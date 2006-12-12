/*_response(c, rs);
 * $Id: http_command.c,v 1.8 2006-12-12 02:36:24 quinn Exp $
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <sys/time.h>

#include <yaz/yaz-util.h>

#include "command.h"
#include "util.h"
#include "eventl.h"
#include "pazpar2.h"
#include "http.h"
#include "http_command.h"

extern struct parameters global_parameters;
extern IOCHAN channel_list;

struct http_session {
    IOCHAN timeout_iochan;     // NOTE: This is NOT associated with a socket
    struct session *psession;
    int session_id;
    int timestamp;
    struct http_session *next;
};

static struct http_session *session_list = 0;

void http_session_destroy(struct http_session *s);

static void session_timeout(IOCHAN i, int event)
{
    struct http_session *s = iochan_getdata(i);
    http_session_destroy(s);
}

struct http_session *http_session_create()
{
    struct http_session *r = xmalloc(sizeof(*r));
    r->psession = new_session();
    r->session_id = 0;
    r->timestamp = 0;
    r->next = session_list;
    session_list = r;
    r->timeout_iochan = iochan_create(-1, session_timeout, 0);
    iochan_setdata(r->timeout_iochan, r);
    iochan_settimeout(r->timeout_iochan, global_parameters.session_timeout);
    r->timeout_iochan->next = channel_list;
    channel_list = r->timeout_iochan;
    return r;
}

void http_session_destroy(struct http_session *s)
{
    struct http_session **p;

    for (p = &session_list; *p; p = &(*p)->next)
        if (*p == s)
        {
            *p = (*p)->next;
            break;
        }
    iochan_destroy(s->timeout_iochan);
    destroy_session(s->psession);
    xfree(s);
}

static void error(struct http_response *rs, char *code, char *msg, char *txt)
{
    struct http_channel *c = rs->channel;
    char tmp[1024];

    if (!txt)
        txt = msg;
    rs->msg = nmem_strdup(c->nmem, msg);
    strcpy(rs->code, code);
    sprintf(tmp, "<error code=\"general\">%s</error>", txt);
    rs->payload = nmem_strdup(c->nmem, tmp);
    http_send_response(c);
}

int make_sessionid()
{
    struct timeval t;
    int res;
    static int seq = 0;

    seq++;
    if (gettimeofday(&t, 0) < 0)
        abort();
    res = t.tv_sec;
    res = (res << 8) | (seq & 0xff);
    return res;
}

static struct http_session *locate_session(struct http_request *rq, struct http_response *rs)
{
    struct http_session *p;
    char *session = http_argbyname(rq, "session");
    int id;

    if (!session)
    {
        error(rs, "417", "Must supply session", 0);
        return 0;
    }
    id = atoi(session);
    for (p = session_list; p; p = p->next)
        if (id == p->session_id)
        {
            iochan_activity(p->timeout_iochan);
            return p;
        }
    error(rs, "417", "Session does not exist, or it has expired", 0);
    return 0;
}

static void cmd_exit(struct http_channel *c)
{
    yaz_log(YLOG_WARN, "exit");
    exit(0);
}


static void cmd_init(struct http_channel *c)
{
    int sesid;
    char buf[1024];
    struct http_session *s = http_session_create();
    struct http_response *rs = c->response;

    // FIXME create a pazpar2 session
    yaz_log(YLOG_DEBUG, "HTTP Session init");
    sesid = make_sessionid();
    s->session_id = sesid;
    sprintf(buf, "<init><status>OK</status><session>%d</session></init>", sesid);
    rs->payload = nmem_strdup(c->nmem, buf);
    http_send_response(c);
}

static void cmd_termlist(struct http_channel *c)
{
    struct http_response *rs = c->response;
    struct http_request *rq = c->request;
    struct http_session *s = locate_session(rq, rs);
    struct termlist_score **p;
    int len;
    int i;

    if (!s)
        return;
    wrbuf_rewind(c->wrbuf);

    wrbuf_puts(c->wrbuf, "<termlist>");
    p = termlist(s->psession, &len);
    if (p)
        for (i = 0; i < len; i++)
        {
            wrbuf_puts(c->wrbuf, "\n<term>");
            wrbuf_printf(c->wrbuf, "<name>%s</name>", p[i]->term);
            wrbuf_printf(c->wrbuf, "<frequency>%d</frequency>", p[i]->frequency);
            wrbuf_puts(c->wrbuf, "</term>");
        }
    wrbuf_puts(c->wrbuf, "</termlist>");
    rs->payload = nmem_strdup(rq->channel->nmem, wrbuf_buf(c->wrbuf));
    http_send_response(c);
}


static void cmd_bytarget(struct http_channel *c)
{
    struct http_response *rs = c->response;
    struct http_request *rq = c->request;
    struct http_session *s = locate_session(rq, rs);
    struct hitsbytarget *ht;
    int count, i;

    if (!s)
        return;
    if (!(ht = hitsbytarget(s->psession, &count)))
    {
        error(rs, "500", "Failed to retrieve hitcounts", 0);
        return;
    }
    wrbuf_rewind(c->wrbuf);
    wrbuf_puts(c->wrbuf, "<bytarget><status>OK</status>");

    for (i = 0; i < count; i++)
    {
        wrbuf_puts(c->wrbuf, "\n<target>");
        wrbuf_printf(c->wrbuf, "<id>%s</id>\n", ht[i].id);
        wrbuf_printf(c->wrbuf, "<hits>%d</hits>\n", ht[i].hits);
        wrbuf_printf(c->wrbuf, "<diagnostic>%d</diagnostic>\n", ht[i].diagnostic);
        wrbuf_printf(c->wrbuf, "<records>%d</records>\n", ht[i].records);
        wrbuf_printf(c->wrbuf, "<state>%s</state>\n", ht[i].state);
        wrbuf_puts(c->wrbuf, "</target>");
    }

    wrbuf_puts(c->wrbuf, "</bytarget>");
    rs->payload = nmem_strdup(c->nmem, wrbuf_buf(c->wrbuf));
    http_send_response(c);
}

static void cmd_show(struct http_channel *c)
{
    struct http_request *rq = c->request;
    struct http_response *rs = c->response;
    struct http_session *s = locate_session(rq, rs);
    struct record **rl;
    char *start = http_argbyname(rq, "start");
    char *num = http_argbyname(rq, "num");
    int startn = 0;
    int numn = 20;
    int total;
    int total_hits;
    int i;

    if (!s)
        return;

    if (start)
        startn = atoi(start);
    if (num)
        numn = atoi(num);

    rl = show(s->psession, startn, &numn, &total, &total_hits);

    wrbuf_rewind(c->wrbuf);
    wrbuf_puts(c->wrbuf, "<show>\n<status>OK</status>\n");
    wrbuf_printf(c->wrbuf, "<merged>%d</merged>\n", total);
    wrbuf_printf(c->wrbuf, "<total>%d</total>\n", total_hits);
    wrbuf_printf(c->wrbuf, "<start>%d</start>\n", startn);
    wrbuf_printf(c->wrbuf, "<num>%d</num>\n", numn);

    for (i = 0; i < numn; i++)
    {
        int ccount;
        struct record *p;

        wrbuf_puts(c->wrbuf, "<hit>\n");
        wrbuf_printf(c->wrbuf, "<title>%s</title>\n", rl[i]->title);
        for (ccount = 1, p = rl[i]->next_cluster; p;  p = p->next_cluster, ccount++)
            ;
        if (ccount > 1)
            wrbuf_printf(c->wrbuf, "<count>%d</count>\n", ccount);
        wrbuf_puts(c->wrbuf, "</hit>\n");
    }

    wrbuf_puts(c->wrbuf, "</show>\n");
    rs->payload = nmem_strdup(c->nmem, wrbuf_buf(c->wrbuf));
    http_send_response(c);
}

static void cmd_ping(struct http_channel *c)
{
    struct http_request *rq = c->request;
    struct http_response *rs = c->response;
    struct http_session *s = locate_session(rq, rs);
    if (!s)
        return;
    rs->payload = "<ping><status>OK</status></ping>";
    http_send_response(c);
}

static void cmd_search(struct http_channel *c)
{
    struct http_request *rq = c->request;
    struct http_response *rs = c->response;
    struct http_session *s = locate_session(rq, rs);
    char *query = http_argbyname(rq, "query");
    char *res;

    if (!s)
        return;
    if (!query)
    {
        error(rs, "417", "Must supply query", 0);
        return;
    }
    res = search(s->psession, query);
    if (res)
    {
        error(rs, "417", res, res);
        return;
    }
    rs->payload = "<search><status>OK</status></search>";
    http_send_response(c);
}


static void cmd_stat(struct http_channel *c)
{
    struct http_request *rq = c->request;
    struct http_response *rs = c->response;
    struct http_session *s = locate_session(rq, rs);
    struct statistics stat;

    if (!s)
        return;

    statistics(s->psession, &stat);

    wrbuf_rewind(c->wrbuf);
    wrbuf_puts(c->wrbuf, "<stat>");
    wrbuf_printf(c->wrbuf, "<hits>%d</hits>\n", stat.num_hits);
    wrbuf_printf(c->wrbuf, "<records>%d</records>\n", stat.num_records);
    wrbuf_printf(c->wrbuf, "<clients>%d</clients>\n", stat.num_clients);
    wrbuf_printf(c->wrbuf, "<unconnected>%d</unconnected>\n", stat.num_no_connection);
    wrbuf_printf(c->wrbuf, "<connecting>%d</connecting>\n", stat.num_connecting);
    wrbuf_printf(c->wrbuf, "<initializing>%d</initializing>\n", stat.num_initializing);
    wrbuf_printf(c->wrbuf, "<searching>%d</searching>\n", stat.num_searching);
    wrbuf_printf(c->wrbuf, "<presenting>%d</presenting>\n", stat.num_presenting);
    wrbuf_printf(c->wrbuf, "<idle>%d</idle>\n", stat.num_idle);
    wrbuf_printf(c->wrbuf, "<failed>%d</failed>\n", stat.num_failed);
    wrbuf_printf(c->wrbuf, "<error>%d</error>\n", stat.num_error);
    wrbuf_puts(c->wrbuf, "</stat>");
    rs->payload = nmem_strdup(c->nmem, wrbuf_buf(c->wrbuf));
    http_send_response(c);
}


struct {
    char *name;
    void (*fun)(struct http_channel *c);
} commands[] = {
    { "init", cmd_init },
    { "stat", cmd_stat },
    { "bytarget", cmd_bytarget },
    { "show", cmd_show },
    { "search", cmd_search },
    { "termlist", cmd_termlist },
    { "exit", cmd_exit },
    { "ping", cmd_ping },
    {0,0}
};

void http_command(struct http_channel *c)
{
    char *command = http_argbyname(c->request, "command");
    struct http_response *rs = http_create_response(c);
    int i;

    c->response = rs;
    if (!command)
    {
        error(rs, "417", "Must supply command", 0);
        return;
    }
    for (i = 0; commands[i].name; i++)
        if (!strcmp(commands[i].name, command))
        {
            (*commands[i].fun)(c);
            break;
        }
    if (!commands[i].name)
        error(rs, "417", "Unknown command", 0);

    return;
}

/*
 * Local variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */
