/*
 * dig and nslookup: real DNS queries over UDP (port 53) to the DNS server
 * from the TCP/IP control panel, or one given with @server / as nslookup's
 * second argument. Messages are built and decoded by dnsmsg.c.
 */
#include "app.h"
#include "console.h"
#include "shell.h"
#include "dnsmsg.h"

#include <Threads.h>
#include <Timer.h>
#include <DateTimeUtils.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void shell_set_status(int idx, int status);

struct dns_job
{
	char name[256];
	char server[64];
	int type;
	int is_dig, short_out, reverse;
};
static struct dns_job jobs[MAX_SESSIONS];

/* a query ID that is hard to guess (microsecond clock mixed with a counter) */
static unsigned short dns_new_id(void)
{
	static unsigned long counter = 0x5A17;
	UnsignedWide t;
	Microseconds(&t);
	counter = counter * 1103515245UL + 12345UL + t.lo + (t.lo >> 16) + TickCount();
	return (unsigned short)((counter >> 16) ^ t.lo);
}

static int default_server(char* out, int size)
{
	InetInterfaceInfo info;
	if (OTInetGetInterfaceInfo(&info, kDefaultInetInterface) != noErr || info.fDNSAddr == 0)
		return 0;
	OTInetHostToString(info.fDNSAddr, out);
	(void)size;
	return 1;
}

/* send the query and wait for the matching reply; returns its length or a negative error */
static long dns_exchange(int idx, InetHost server, const unsigned char* q, long qlen,
                         unsigned char* reply, long size, unsigned long* ms)
{
	struct session* s = &sessions[idx];
	EndpointRef ep;
	OSStatus err;
	TUnitData ud;
	InetAddress to, from;
	int attempt;
	long got = -1;
	UnsignedWide t0, t1;

	ep = OTOpenEndpoint(OTCreateConfiguration(kUDPName), 0, nil, &err);
	if (err != noErr) return -2;
	if (OTBind(ep, nil, nil) != noErr) { OTCloseProvider(ep); return -2; }
	OTSetNonBlocking(ep);

	for (attempt = 0; attempt < 3 && got < 0; attempt++)
	{
		unsigned long start = TickCount();
		OTInitInetAddress(&to, 53, server);
		OTMemzero(&ud, sizeof ud);
		ud.addr.buf = (UInt8*)&to;
		ud.addr.len = sizeof to;
		ud.udata.buf = (UInt8*)q;
		ud.udata.len = qlen;
		Microseconds(&t0);
		if (OTSndUData(ep, &ud) != noErr) break;
		while (TickCount() - start < 3 * 60)                    /* 3 seconds */
		{
			OTFlags flags = 0;
			OTResult r;
			if (s->thread_command == EXIT || !s->in_use) { got = -3; break; }
			OTMemzero(&ud, sizeof ud);
			ud.addr.buf = (UInt8*)&from;
			ud.addr.maxlen = sizeof from;
			ud.udata.buf = reply;
			ud.udata.maxlen = size;
			r = OTRcvUData(ep, &ud, &flags);
			if (r == kOTNoDataErr) { YieldToAnyThread(); continue; }
			if (r == kOTLookErr) { if (OTLook(ep) == T_UDERR) OTRcvUDErr(ep, NULL); continue; }
			if (r != noErr) { YieldToAnyThread(); continue; }
			/* only the server we asked, from port 53, answering our ID,
			   and only a response (QR bit set) */
			if (ud.udata.len >= 12 && reply[0] == q[0] && reply[1] == q[1] &&
			    (reply[2] & 0x80) && from.fHost == server && from.fPort == 53)
			{
				Microseconds(&t1);
				*ms = (t1.lo - t0.lo) / 1000;
				got = ud.udata.len;
				break;
			}
		}
		if (got == -3) break;
	}
	OTUnbind(ep);
	OTCloseProvider(ep);
	return got;
}

/* Looks up a host name's IPv4 address for the other network commands, with
   our own query to the control panel's DNS server, so the time it can take is
   known (at most three tries of 3 seconds) and Ctrl+C (thread_command = EXIT)
   stops it. A dotted address is converted without a lookup.
   Returns 0 and sets *addr, or prints why not ("cmd: ...") and returns -1
   (-3 if cancelled). */
int dns_resolve(int idx, const char* cmd, const char* name, InetHost* addr)
{
	char server_s[64];
	InetHost server;
	unsigned char q[300];
	unsigned char* reply = NULL;
	struct dns_reply* r = NULL;
	long qlen, n;
	unsigned long ms;
	int i, ret = -1, depth;
	char qname[256];

	if (OTInetStringToHost((char*)name, addr) == noErr)
		return 0;
	if (!default_server(server_s, sizeof server_s) || OTInetStringToHost(server_s, &server) != noErr)
	{
		printf_s(idx, "%s: no DNS server set in the TCP/IP control panel\r\n", cmd);
		return -1;
	}
	reply = (unsigned char*)NewPtr(1500);
	r = (struct dns_reply*)NewPtr(sizeof(struct dns_reply));
	if (reply == NULL || r == NULL)
	{
		printf_s(idx, "%s: out of memory\r\n", cmd);
		goto done;
	}
	snprintf(qname, sizeof qname, "%s", name);
	for (depth = 0; depth < 4; depth++)                         /* follow CNAMEs */
	{
		int cname = 0;
		qlen = dns_build_query(q, sizeof q, dns_new_id(), qname, DNS_A, 1);
		if (qlen < 0)
		{
			printf_s(idx, "%s: invalid host name: %s\r\n", cmd, name);
			goto done;
		}
		n = dns_exchange(idx, server, q, qlen, reply, 1500, &ms);
		if (n == -3) { ret = -3; goto done; }
		if (n < 0)
		{
			printf_s(idx, "%s: no answer from the DNS server (%s)\r\n", cmd, server_s);
			goto done;
		}
		if (dns_parse_reply(reply, (int)n, r) != 0 || r->rcode != 0)
		{
			if (r->rcode == 3)
				printf_s(idx, "%s: unknown host: %s\r\n", cmd, name);
			else
				printf_s(idx, "%s: can't look up %s\r\n", cmd, name);
			goto done;
		}
		for (i = 0; i < r->count; i++)
			if (r->rr[i].section == 1 && r->rr[i].type == DNS_A &&
			    OTInetStringToHost(r->rr[i].data, addr) == noErr)
			{
				ret = 0;
				goto done;
			}
		for (i = 0; i < r->count; i++)
			if (r->rr[i].section == 1 && r->rr[i].type == DNS_CNAME)
			{
				snprintf(qname, sizeof qname, "%s", r->rr[i].data);
				cname = 1;
				break;
			}
		if (!cname) break;
	}
	printf_s(idx, "%s: unknown host: %s\r\n", cmd, name);
done:
	if (reply) DisposePtr((Ptr)reply);
	if (r) DisposePtr((Ptr)r);
	return ret;
}

static void print_rr(int idx, const struct dns_rr* rr, int short_out)
{
	if (short_out)
		printf_s(idx, "%s\r\n", rr->data);
	else
		printf_s(idx, "%-23s %-7lu IN      %-7s %s\r\n", rr->name, rr->ttl, dns_type_name(rr->type), rr->data);
}

static void* dns_thread(void* arg)
{
	int idx = (int)(long)arg;
	struct session* s = &sessions[idx];
	struct dns_job* j = &jobs[idx];
	unsigned char q[300];
	unsigned char* reply = NULL;
	struct dns_reply* r = NULL;
	InetHost server = 0;
	long qlen, n;
	unsigned long ms = 0;
	int status = 1, i, sec;
	char qname[256];

	if (j->server[0] == '\0' && !default_server(j->server, sizeof j->server))
	{
		printf_s(idx, "%s: no DNS server set in the TCP/IP control panel\r\n", j->is_dig ? "dig" : "nslookup");
		goto done;
	}
	if (OTInetStringToHost(j->server, &server) != noErr)
	{
		printf_s(idx, "%s: the server must be an IP address: %s\r\n", j->is_dig ? "dig" : "nslookup", j->server);
		goto done;
	}
	if (j->reverse)
	{
		if (!dns_reverse_name(j->name, qname, sizeof qname))
		{
			printf_s(idx, "%s: not an IPv4 address: %s\r\n", j->is_dig ? "dig" : "nslookup", j->name);
			goto done;
		}
	}
	else
		snprintf(qname, sizeof qname, "%s", j->name);

	qlen = dns_build_query(q, sizeof q, dns_new_id(), qname, j->type, 1);
	if (qlen < 0)
	{
		printf_s(idx, "%s: invalid name: %s\r\n", j->is_dig ? "dig" : "nslookup", qname);
		goto done;
	}
	reply = (unsigned char*)NewPtr(4096);
	r = (struct dns_reply*)NewPtr(sizeof(struct dns_reply));
	if (reply == NULL || r == NULL)
	{
		printf_s(idx, "%s: out of memory\r\n", j->is_dig ? "dig" : "nslookup");
		goto done;
	}
	n = dns_exchange(idx, server, q, qlen, reply, 4096, &ms);
	if (n == -3) { status = 130; goto done; }
	if (n < 0)
	{
		printf_s(idx, j->is_dig ? ";; connection timed out; no servers could be reached\r\n"
		                        : ";; connection timed out; no servers could be reached\r\n");
		goto done;
	}
	if (dns_parse_reply(reply, n, r) != 0)
	{
		printf_s(idx, "%s: malformed reply from %s\r\n", j->is_dig ? "dig" : "nslookup", j->server);
		goto done;
	}

	if (j->is_dig)
	{
		if (j->short_out)
		{
			for (i = 0; i < r->count; i++)
				if (r->rr[i].section == 1) print_rr(idx, &r->rr[i], 1);
		}
		else
		{
			unsigned long secs;
			DateTimeRec dt;
			static const char* mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
			static const char* wd[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
			printf_s(idx, "; <<>> MacShell dig <<>> %s %s\r\n", j->name, dns_type_name(j->type));
			printf_s(idx, ";; ->>HEADER<<- opcode: QUERY, status: %s, id: %u\r\n", dns_rcode_name(r->rcode), r->id);
			printf_s(idx, ";; flags:%s%s%s%s%s; QUERY: %d, ANSWER: %d, AUTHORITY: %d, ADDITIONAL: %d\r\n",
			         (r->flags & 0x8000) ? " qr" : "", (r->flags & 0x0400) ? " aa" : "",
			         (r->flags & 0x0200) ? " tc" : "", (r->flags & 0x0100) ? " rd" : "",
			         (r->flags & 0x0080) ? " ra" : "", r->qdcount, r->ancount, r->nscount, r->arcount);
			printf_s(idx, "\r\n;; QUESTION SECTION:\r\n;%-22s          IN      %s\r\n", r->qname, dns_type_name(r->qtype));
			for (sec = 1; sec <= 3; sec++)
			{
				int any = 0;
				for (i = 0; i < r->count; i++)
				{
					if (r->rr[i].section != sec) continue;
					if (!any)
						printf_s(idx, "\r\n;; %s SECTION:\r\n", sec == 1 ? "ANSWER" : sec == 2 ? "AUTHORITY" : "ADDITIONAL");
					any = 1;
					print_rr(idx, &r->rr[i], 0);
				}
			}
			GetDateTime(&secs);
			SecondsToDate(secs, &dt);
			printf_s(idx, "\r\n;; Query time: %lu msec\r\n;; SERVER: %s#53(%s) (UDP)\r\n", ms, j->server, j->server);
			printf_s(idx, ";; WHEN: %s %s %d %02d:%02d:%02d %d\r\n;; MSG SIZE  rcvd: %ld\r\n",
			         wd[(dt.dayOfWeek + 6) % 7], mon[dt.month - 1], dt.day, dt.hour, dt.minute, dt.second, dt.year, n);
			if (r->flags & 0x0200)
				printf_s(idx, ";; Truncated reply (the full answer needs TCP, which MacShell's dig doesn't use)\r\n");
		}
		status = r->rcode == 0 ? 0 : 1;
	}
	else
	{
		printf_s(idx, "Server:\t\t%s\r\nAddress:\t%s#53\r\n\r\n", j->server, j->server);
		if (r->rcode != 0)
		{
			printf_s(idx, "** server can't find %s: %s\r\n", j->name, dns_rcode_name(r->rcode));
			goto done;
		}
		if (!(r->flags & 0x0400)) printf_s(idx, "Non-authoritative answer:\r\n");
		for (i = 0; i < r->count; i++)
		{
			const struct dns_rr* rr = &r->rr[i];
			char nm[256];
			int l;
			if (rr->section != 1) continue;
			snprintf(nm, sizeof nm, "%s", rr->name);
			l = strlen(nm);
			if (l > 1 && nm[l - 1] == '.') nm[l - 1] = '\0';
			switch (rr->type)
			{
			case DNS_A: case DNS_AAAA: printf_s(idx, "Name:\t%s\r\nAddress: %s\r\n", nm, rr->data); break;
			case DNS_CNAME: printf_s(idx, "%s\tcanonical name = %s\r\n", nm, rr->data); break;
			case DNS_MX: printf_s(idx, "%s\tmail exchanger = %s\r\n", nm, rr->data); break;
			case DNS_NS: printf_s(idx, "%s\tnameserver = %s\r\n", nm, rr->data); break;
			case DNS_PTR: printf_s(idx, "%s\tname = %s\r\n", nm, rr->data); break;
			case DNS_TXT: printf_s(idx, "%s\ttext = %s\r\n", nm, rr->data); break;
			default: printf_s(idx, "%s\t%s = %s\r\n", nm, dns_type_name(rr->type), rr->data); break;
			}
		}
		if (r->ancount == 0)
			printf_s(idx, "*** No %s records found for %s\r\n", dns_type_name(j->type), j->name);
		status = 0;
	}

done:
	if (reply) DisposePtr((Ptr)reply);
	if (r) DisposePtr((Ptr)r);
	shell_set_status(idx, status);
	s->worker_mode = WORKER_NONE;
	s->thread_state = DONE;
	s->thread_command = WAIT;
	if (s->in_use && s->type == SESSION_LOCAL)
		shell_prompt(idx);
	return 0;
}

static void dns_start(int idx)
{
	struct session* s = &sessions[idx];
	ThreadID tid = kNoThreadID;
	OSErr err;

	if (InitOpenTransport() != noErr)
	{
		printf_s(idx, "%s: Open Transport not available\r\n", jobs[idx].is_dig ? "dig" : "nslookup");
		return;
	}
	if (s->thread_state == DONE && s->thread_id != kNoThreadID)
		session_reap_thread(idx, 0);
	s->thread_command = READ;
	s->thread_state = OPEN;
	err = NewThread(kCooperativeThread, dns_thread, (void*)(long)idx, THREAD_STACK_WORKER,
	                kCreateIfNeeded, NULL, &tid);
	if (err != noErr)
	{
		s->thread_command = WAIT;
		s->thread_state = DONE;
		s->thread_id = kNoThreadID;
		printf_s(idx, "dns: cannot start (err=%d)\r\n", (int)err);
		return;
	}
	s->thread_id = tid;
	s->worker_mode = WORKER_PING;
}

/* dig [@server] [-x address] name [type] [+short] */
void cmd_dig(int idx, int argc, char* argv[])
{
	struct dns_job* j = &jobs[idx];
	int i;
	memset(j, 0, sizeof *j);
	j->is_dig = 1;
	j->type = DNS_A;
	for (i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		if (a[0] == '@') snprintf(j->server, sizeof j->server, "%s", a + 1);
		else if (strcmp(a, "-x") == 0 && i + 1 < argc) { snprintf(j->name, sizeof j->name, "%s", argv[++i]); j->reverse = 1; j->type = DNS_PTR; }
		else if (strcmp(a, "+short") == 0) j->short_out = 1;
		else if (a[0] == '+') { }                               /* other dig +options: ignored */
		else if (j->name[0] && !j->reverse && dns_type_from_name(a)) j->type = dns_type_from_name(a);
		else if (!j->name[0]) snprintf(j->name, sizeof j->name, "%s", a);
		else if (dns_type_from_name(a)) j->type = dns_type_from_name(a);
		else { printf_s(idx, "dig: unknown record type %s\r\n", a); return; }
	}
	if (!j->name[0])
	{
		printf_s(idx, "usage: dig [@server] <name> [A|AAAA|MX|NS|TXT|CNAME|SOA|PTR] [+short]\r\n"
		              "       dig [@server] -x <address>\r\n");
		return;
	}
	dns_start(idx);
}

/* nslookup [-type=X] name [server] */
void cmd_nslookup(int idx, int argc, char* argv[])
{
	struct dns_job* j = &jobs[idx];
	int i;
	memset(j, 0, sizeof *j);
	j->type = DNS_A;
	for (i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		if (strncmp(a, "-type=", 6) == 0 || strncmp(a, "-query=", 7) == 0 || strncmp(a, "-q=", 3) == 0)
		{
			const char* t = strchr(a, '=') + 1;
			j->type = dns_type_from_name(t);
			if (!j->type) { printf_s(idx, "nslookup: unknown record type %s\r\n", t); return; }
		}
		else if (!j->name[0]) snprintf(j->name, sizeof j->name, "%s", a);
		else snprintf(j->server, sizeof j->server, "%s", a);
	}
	if (!j->name[0])
	{
		printf_s(idx, "usage: nslookup [-type=A|AAAA|MX|NS|TXT|CNAME|SOA|PTR] <name> [server]\r\n");
		return;
	}
	/* an address: look up its name, as nslookup does */
	{
		char tmp[64];
		if (j->type == DNS_A && dns_reverse_name(j->name, tmp, sizeof tmp))
		{
			j->reverse = 1;
			j->type = DNS_PTR;
		}
	}
	dns_start(idx);
}
