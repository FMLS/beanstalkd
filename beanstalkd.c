/* beanstalk - fast, general-purpose work queue */

#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "conn.h"
#include "net.h"
#include "beanstalkd.h"
#include "pq.h"
#include "util.h"
#include "prot.h"
#include "reserve.h"

/* job body cannot be greater than this */
#define JOB_DATA_SIZE_LIMIT ((1 << 16) - 1)

static unsigned long long int put_ct = 0, peek_ct = 0, reserve_ct = 0,
                     delete_ct = 0, release_ct = 0, stats_ct = 0,
                     timeout_ct = 0;

static void
drop_root()
{
    /* pass */
}

static void
daemonize()
{
    /* pass */
}

static void
set_sig_handlers()
{
    int r;
    struct sigaction sa;

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    r = sigemptyset(&sa.sa_mask);
    if (r == -1) perror("sigemptyset()"), exit(111);

    r = sigaction(SIGPIPE, &sa, 0);
    if (r == -1) perror("sigaction(SIGPIPE)"), exit(111);
}

static void
check_err(conn c, const char *s)
{
    if (errno == EAGAIN) return;
    if (errno == EINTR) return;
    if (errno == EWOULDBLOCK) return;

    perror(s);
    conn_close(c);
    return;
}

/* Scan the given string for the sequence "\r\n" and return the line length.
 * Always returns at least 2 if a match is found. Returns 0 if no match. */
static int
scan_line_end(const char *s, int size)
{
    char *match;

    match = memchr(s, '\r', size - 1);
    if (!match) return 0;

    /* this is safe because we only scan size - 1 chars above */
    if (match[1] == '\n') return match - s + 2;

    return 0;
}

static int
cmd_len(conn c)
{
    return scan_line_end(c->cmd, c->cmd_read);
}

/* parse the command line */
static int
which_cmd(conn c)
{
#define TEST_CMD(s,c,o) if (strncmp((s), (c), CONSTSTRLEN(c)) == 0) return (o);
    TEST_CMD(c->cmd, CMD_PUT, OP_PUT);
    TEST_CMD(c->cmd, CMD_PEEK, OP_PEEK);
    TEST_CMD(c->cmd, CMD_RESERVE, OP_RESERVE);
    TEST_CMD(c->cmd, CMD_DELETE, OP_DELETE);
    TEST_CMD(c->cmd, CMD_RELEASE, OP_RELEASE);
    TEST_CMD(c->cmd, CMD_JOBSTATS, OP_JOBSTATS);
    TEST_CMD(c->cmd, CMD_STATS, OP_STATS);
    return OP_UNKNOWN;
}

/* Copy up to body_size trailing bytes into the job, then the rest into the cmd
 * buffer. If c->in_job exists, this assumes that c->in_job->body is empty.
 * This function is idempotent(). */
static void
fill_extra_data(conn c)
{
    int extra_bytes, job_data_bytes = 0, cmd_bytes;

    if (!c->fd) return; /* the connection was closed */
    if (!c->cmd_len) return; /* we don't have a complete command */

    /* how many extra bytes did we read? */
    extra_bytes = c->cmd_read - c->cmd_len;

    /* how many bytes should we put into the job body? */
    if (c->in_job) {
        job_data_bytes = min(extra_bytes, c->in_job->body_size);
        memcpy(c->in_job->body, c->cmd + c->cmd_len, job_data_bytes);
        c->in_job_read = job_data_bytes;
    }

    /* how many bytes are left to go into the future cmd? */
    cmd_bytes = extra_bytes - job_data_bytes;
    memmove(c->cmd, c->cmd + c->cmd_len + job_data_bytes, cmd_bytes);
    c->cmd_read = cmd_bytes;
    c->cmd_len = 0; /* we no longer know the length of the new command */
}

static void
enqueue_incoming_job(conn c)
{
    int r;
    job j = c->in_job;

    c->in_job = NULL; /* the connection no longer owns this job */
    c->in_job_read = 0;

    /* check if the trailer is present and correct */
    if (memcmp(j->body + j->body_size - 2, "\r\n", 2)) return conn_close(c);

    /* we have a complete job, so let's stick it in the pqueue */
    r = enqueue_job(j);

    if (r) return reply(c, MSG_INSERTED, MSG_INSERTED_LEN, STATE_SENDWORD);

    free(j); /* the job didn't go in the queue, so it goes bye bye */
    reply(c, MSG_NOT_INSERTED, MSG_NOT_INSERTED_LEN, STATE_SENDWORD);
}

static void
maybe_enqueue_incoming_job(conn c)
{
    job j = c->in_job;

    /* do we have a complete job? */
    if (c->in_job_read == j->body_size) return enqueue_incoming_job(c);

    /* otherwise we have incomplete data, so just keep waiting */
    c->state = STATE_WANTDATA;
}

static void
wait_for_job(conn c)
{
    int r;

    /* this conn is waiting, but we want to know if they hang up */
    r = conn_update_evq(c, EV_READ | EV_PERSIST);
    if (r == -1) return warn("update events failed"), conn_close(c);

    c->state = STATE_WAIT;
    enqueue_waiting_conn(c);
}

static int
fmt_stats(char *buf, size_t size, void *x)
{
    return snprintf(buf, size, STATS_FMT,
            get_ready_job_ct(),
            get_reserved_job_ct(),
            put_ct,
            peek_ct,
            reserve_ct,
            delete_ct,
            stats_ct,
            timeout_ct,
            count_cur_conns(),
            count_cur_producers(),
            count_cur_workers());

}

static int
fmt_job_stats(char *buf, size_t size, void *jp)
{
    time_t t;
    job j = (job) jp;

    t = time(NULL);
    return snprintf(buf, size, JOB_STATS_FMT,
            j->id,
            job_state(j),
            (unsigned int) (t - j->creation),
            (unsigned int) (j->deadline - t),
            j->timeout_ct);

}

static void
do_stats(conn c, int(*fmt)(char *, size_t, void *), void *data)
{
    int r, stats_len;

    /* first, measure how big a buffer we will need */
    stats_len = fmt(NULL, 0, data);

    c->out_job = allocate_job(stats_len); /* fake job to hold stats data */
    if (!c->out_job) return conn_close(c);

    /* now actually format the stats data */
    r = fmt(c->out_job->body, stats_len, data);
    if (r != stats_len) return warn("snprintf inconsistency"), conn_close(c);
    c->out_job->body[stats_len - 1] = '\n'; /* patch up sprintf's output */

    c->out_job_sent = 0;

    r = snprintf(c->reply_buf, LINE_BUF_SIZE, "OK %d\r\n", stats_len - 2);
    if (r >= LINE_BUF_SIZE) return warn("truncated reply"), conn_close(c);

    reply(c, c->reply_buf, strlen(c->reply_buf), STATE_SENDJOB);
}

static void
dispatch_cmd(conn c)
{
    int r;
    job j;
    char type;
    char *size_buf, *end_buf;
    unsigned int pri, body_size;
    unsigned long long int id;

    /* NUL-terminate this string so we can use strtol and friends */
    c->cmd[c->cmd_len - 2] = '\0';

    /* check for possible maliciousness */
    if (strlen(c->cmd) != c->cmd_len - 2) return conn_close(c);

    type = which_cmd(c);

    switch (type) {
    case OP_PUT:
        errno = 0;
        pri = strtoul(c->cmd + 4, &size_buf, 10);
        if (errno) return conn_close(c);

        errno = 0;
        body_size = strtoul(size_buf, &end_buf, 10);
        if (errno) return conn_close(c);

        if (body_size > JOB_DATA_SIZE_LIMIT) return conn_close(c);

        /* don't allow trailing garbage */
        if (end_buf[0] != '\0') return conn_close(c);

        put_ct++; /* stats */
        conn_set_producer(c);

        c->in_job = make_job(pri, body_size + 2);

        fill_extra_data(c);

        /* it's possible we already have a complete job */
        maybe_enqueue_incoming_job(c);

        break;
    case OP_PEEK:
        errno = 0;
        id = strtoull(c->cmd + CMD_PEEK_LEN, &end_buf, 10);
        if (errno) return conn_close(c);

        peek_ct++; /* stats */

        /* So, peek is annoying, because some other connection might free the
         * job while we are still trying to write it out. So we copy it and
         * then free the copy when it's done sending. */
        j = job_copy(peek_job(id));

        if (j) return reply_job(c, j, MSG_FOUND);

        reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);
        break;
    case OP_RESERVE:
        /* don't allow trailing garbage */
        if (c->cmd_len != CMD_RESERVE_LEN + 2) return conn_close(c);

        reserve_ct++; /* stats */
        conn_set_worker(c);

        /* try to get a new job for this guy */
        wait_for_job(c);
        process_queue();
        break;
    case OP_DELETE:
        errno = 0;
        id = strtoull(c->cmd + CMD_DELETE_LEN, &end_buf, 10);
        if (errno) return conn_close(c);

        delete_ct++; /* stats */

        j = remove_reserved_job(c, id);

        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        free(j);

        reply(c, MSG_DELETED, MSG_DELETED_LEN, STATE_SENDWORD);
        break;
    case OP_RELEASE:
        errno = 0;
        id = strtoull(c->cmd + CMD_RELEASE_LEN, &end_buf, 10);
        if (errno) return conn_close(c);

        release_ct++; /* stats */

        j = remove_reserved_job(c, id);

        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);

        r = enqueue_job(j);
        if (r) return reply(c, MSG_RELEASED, MSG_RELEASED_LEN, STATE_SENDWORD);

        free(j); /* the job didn't go in the queue, so it goes bye bye */
        reply(c, MSG_NOT_RELEASED, MSG_NOT_RELEASED_LEN, STATE_SENDWORD);
        break;
    case OP_STATS:
        /* don't allow trailing garbage */
        if (c->cmd_len != CMD_STATS_LEN + 2) return conn_close(c);

        stats_ct++; /* stats */

        do_stats(c, fmt_stats, NULL);
        break;
    case OP_JOBSTATS:
        errno = 0;
        id = strtoull(c->cmd + CMD_JOBSTATS_LEN, &end_buf, 10);
        if (errno) return conn_close(c);

        stats_ct++; /* stats */

        j = peek_job(id);
        if (!j) return reply(c, MSG_NOTFOUND, MSG_NOTFOUND_LEN, STATE_SENDWORD);
        do_stats(c, fmt_job_stats, j);
        break;
    default:
        /* unknown command -- protocol error */
        return conn_close(c);
    }
}

static void
do_cmd(conn c)
{
    dispatch_cmd(c);
    fill_extra_data(c);
}

static void
reset_conn(conn c)
{
    int r;

    r = conn_update_evq(c, EV_READ | EV_PERSIST);
    if (r == -1) return warn("update events failed"), conn_close(c);

    /* was this a peek or stats command? */
    if (!has_reserved_this_job(c, c->out_job)) free(c->out_job);
    c->out_job = NULL;

    c->reply_sent = 0; /* now that we're done, reset this */
    c->state = STATE_WANTCOMMAND;
}

static void
h_conn_data(conn c)
{
    int r;
    job j;
    struct iovec iov[2];

    switch (c->state) {
    case STATE_WANTCOMMAND:
        r = read(c->fd, c->cmd + c->cmd_read, LINE_BUF_SIZE - c->cmd_read);
        if (r == -1) return check_err(c, "read()");
        if (r == 0) return conn_close(c); /* the client hung up */

        c->cmd_read += r; /* we got some bytes */

        c->cmd_len = cmd_len(c); /* find the EOL */

        /* yay, complete command line */
        if (c->cmd_len) return do_cmd(c);

        /* c->cmd_read > LINE_BUF_SIZE can't happen */

        /* command line too long? */
        if (c->cmd_read == LINE_BUF_SIZE) return conn_close(c);

        /* otherwise we have an incomplete line, so just keep waiting */
        break;
    case STATE_WANTDATA:
        j = c->in_job;

        r = read(c->fd, j->body + c->in_job_read, j->body_size - c->in_job_read);
        if (r == -1) return check_err(c, "read()");
        if (r == 0) return conn_close(c); /* the client hung up */

        c->in_job_read += r; /* we got some bytes */

        /* (j->in_job_read > j->body_size) can't happen */

        maybe_enqueue_incoming_job(c);
        break;
    case STATE_SENDWORD:
        r= write(c->fd, c->reply + c->reply_sent, c->reply_len - c->reply_sent);
        if (r == -1) return check_err(c, "write()");
        if (r == 0) return conn_close(c); /* the client hung up */

        c->reply_sent += r; /* we got some bytes */

        /* (c->reply_sent > c->reply_len) can't happen */

        if (c->reply_sent == c->reply_len) return reset_conn(c);

        /* otherwise we sent an incomplete reply, so just keep waiting */
        break;
    case STATE_SENDJOB:
        j = c->out_job;

        iov[0].iov_base = c->reply + c->reply_sent;
        iov[0].iov_len = c->reply_len - c->reply_sent; /* maybe 0 */
        iov[1].iov_base = j->body + c->out_job_sent;
        iov[1].iov_len = j->body_size - c->out_job_sent;

        r = writev(c->fd, iov, 2);
        if (r == -1) return check_err(c, "writev()");
        if (r == 0) return conn_close(c); /* the client hung up */

        /* update the sent values */
        c->reply_sent += r;
        if (c->reply_sent >= c->reply_len) {
            c->out_job_sent += c->reply_sent - c->reply_len;
            c->reply_sent = c->reply_len;
        }

        /* (c->out_job_sent > j->body_size) can't happen */

        /* are we done? */
        if (c->out_job_sent == j->body_size) return reset_conn(c);

        /* otherwise we sent incomplete data, so just keep waiting */
        break;
    case STATE_WAIT: /* keep an eye out in case they hang up */
        /* but don't hang up just because our buffer is full */
        if (LINE_BUF_SIZE - c->cmd_read < 1) break;

        r = read(c->fd, c->cmd + c->cmd_read, LINE_BUF_SIZE - c->cmd_read);
        if (r == -1) return check_err(c, "read()");
        if (r == 0) return conn_close(c); /* the client hung up */
        c->cmd_read += r; /* we got some bytes */
    }
}

/* if we get a timeout, it means that a job has been reserved for too long, so
 * we should put it back in the queue */
static void
h_conn_timeout(conn c)
{
    int r;
    job j;

    while ((j = soonest_job(c))) {
        if (j->deadline > time(NULL)) return;
        timeout_ct++; /* stats */
        j->timeout_ct++;
        enqueue_job(remove_this_reserved_job(c, j));
        r = conn_update_evq(c, c->evq.ev_events);
        if (r == -1) return warn("conn_update_evq() failed"), conn_close(c);
    }
}

#define want_command(c) ((c)->fd && ((c)->state == STATE_WANTCOMMAND))
#define cmd_data_ready(c) (want_command(c) && (c)->cmd_read)

static void
h_conn(const int fd, const short which, conn c)
{
    if (fd != c->fd) {
        warn("Argh! event fd doesn't match conn fd.");
        close(fd);
        return conn_close(c);
    }

    switch (which) {
    case EV_TIMEOUT:
        h_conn_timeout(c);
        event_add(&c->evq, NULL); /* seems to be necessary */
        break;
    case EV_READ:
        /* fall through... */
    case EV_WRITE:
        /* fall through... */
    default:
        h_conn_data(c);
    }

    while (cmd_data_ready(c) && (c->cmd_len = cmd_len(c))) do_cmd(c);
}

static void
h_accept(const int fd, const short which, struct event *ev)
{
    conn c;
    int cfd, flags, r;
    socklen_t addrlen;
    struct sockaddr addr;

    addrlen = sizeof addr;
    cfd = accept(fd, &addr, &addrlen);
    if (cfd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept()");
        return;
    }

    flags = fcntl(cfd, F_GETFL, 0);
    if (flags < 0) return perror("getting flags"), close(cfd), v();

    r = fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
    if (r < 0) return perror("setting O_NONBLOCK"), close(cfd), v();

    c = make_conn(cfd, STATE_WANTCOMMAND);
    if (!c) return warn("make_conn() failed"), close(cfd), v();

    r = conn_set_evq(c, EV_READ | EV_PERSIST, (evh) h_conn);
    if (r == -1) return warn("conn_set_evq() failed"), close(cfd), v();
}

int
main(int argc, char **argv)
{
    int listen_socket;
    struct event listen_evq;

    listen_socket = make_server_socket(HOST, PORT);
    if (listen_socket == -1) warn("make_server_socket()"), exit(111);

    drop_root();
    prot_init();
    daemonize();
    event_init();
    set_sig_handlers();

    event_set(&listen_evq, listen_socket, EV_READ | EV_PERSIST, (evh) h_accept, &listen_evq);
    event_add(&listen_evq, NULL);

    event_dispatch();
    return 0;
}
