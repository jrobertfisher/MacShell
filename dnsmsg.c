/*
 * DNS messages for dig and nslookup (RFC 1035): building queries and
 * decoding replies. Every length and pointer in a reply is checked, since
 * the reply comes from the network.
 */
#include "dnsmsg.h"

#include <stdio.h>
#include <string.h>

static unsigned short be16(const unsigned char* p) { return (unsigned short)((p[0] << 8) | p[1]); }
static unsigned long be32(const unsigned char* p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

int dns_build_query(unsigned char* buf, int size, unsigned short id,
                    const char* name, unsigned short type, int recurse)
{
	int n = 12;
	const char* p = name;

	if (size < 18) return -1;
	memset(buf, 0, 12);
	buf[0] = (unsigned char)(id >> 8);
	buf[1] = (unsigned char)id;
	buf[2] = recurse ? 0x01 : 0x00;                     /* RD */
	buf[5] = 1;                                         /* one question */

	if (strcmp(name, ".") == 0) p = "";
	while (*p)
	{
		const char* dot = strchr(p, '.');
		int len = dot ? (int)(dot - p) : (int)strlen(p);
		if (len == 0 || len > 63) return -1;
		if (n + 1 + len + 5 >= size || n + len > 255 + 12) return -1;
		buf[n++] = (unsigned char)len;
		memcpy(buf + n, p, len);
		n += len;
		p += len;
		if (*p == '.') p++;
	}
	buf[n++] = 0;
	buf[n++] = (unsigned char)(type >> 8);
	buf[n++] = (unsigned char)type;
	buf[n++] = 0;
	buf[n++] = 1;                                       /* class IN */
	return n;
}

/* read a (possibly compressed) name at *pos; returns 0 or -1 */
static int read_name(const unsigned char* msg, int len, int* pos, char* out, int size)
{
	int p = *pos, o = 0, jumped = 0, hops = 0;

	out[0] = '\0';
	while (1)
	{
		int l;
		if (p >= len) return -1;
		l = msg[p];
		if ((l & 0xC0) == 0xC0)
		{
			int target;
			if (p + 1 >= len || ++hops > 32) return -1;     /* no pointer loops */
			target = ((l & 0x3F) << 8) | msg[p + 1];
			if (target >= len) return -1;
			if (!jumped) *pos = p + 2;
			jumped = 1;
			p = target;
			continue;
		}
		if (l & 0xC0) return -1;
		if (l == 0)
		{
			if (!jumped) *pos = p + 1;
			break;
		}
		if (p + 1 + l > len) return -1;
		if (o + l + 2 >= size) return -1;
		{
			int i;
			for (i = 0; i < l; i++)
			{
				unsigned char c = msg[p + 1 + i];
				out[o++] = (c > 32 && c < 127) ? (char)c : '?';
			}
		}
		out[o++] = '.';
		p += 1 + l;
	}
	if (o == 0) out[o++] = '.';
	out[o] = '\0';
	return 0;
}

static void ipv6_text(const unsigned char* a, char* out, int size)
{
	unsigned short g[8];
	int i, best = -1, best_len = 0, run = -1, run_len = 0, o = 0;
	for (i = 0; i < 8; i++) g[i] = be16(a + 2 * i);
	for (i = 0; i < 8; i++)
	{
		if (g[i] == 0)
		{
			if (run < 0) { run = i; run_len = 0; }
			run_len++;
			if (run_len > best_len) { best = run; best_len = run_len; }
		}
		else
			run = -1;
	}
	if (best_len < 2) best = -1;
	for (i = 0; i < 8 && o < size - 6; i++)
	{
		if (i == best)
		{
			o += snprintf(out + o, size - o, i == 0 ? "::" : ":");
			i += best_len - 1;
			continue;
		}
		o += snprintf(out + o, size - o, "%x%s", g[i], i < 7 ? ":" : "");
	}
	out[o < size ? o : size - 1] = '\0';
}

/* format a record's data as dig does */
static int format_rdata(const unsigned char* msg, int len, int pos, int rdlen, unsigned short type,
                        char* out, int size)
{
	int end = pos + rdlen, p = pos;
	char n1[256], n2[256];

	out[0] = '\0';
	switch (type)
	{
	case DNS_A:
		if (rdlen != 4) return -1;
		snprintf(out, size, "%d.%d.%d.%d", msg[p], msg[p + 1], msg[p + 2], msg[p + 3]);
		return 0;
	case DNS_AAAA:
		if (rdlen != 16) return -1;
		ipv6_text(msg + p, out, size);
		return 0;
	case DNS_NS: case DNS_CNAME: case DNS_PTR:
		if (read_name(msg, len, &p, out, size) != 0) return -1;
		return 0;
	case DNS_MX:
		if (rdlen < 3) return -1;
		p += 2;
		if (read_name(msg, len, &p, n1, sizeof n1) != 0) return -1;
		snprintf(out, size, "%d %s", be16(msg + pos), n1);
		return 0;
	case DNS_SOA:
		if (read_name(msg, len, &p, n1, sizeof n1) != 0) return -1;
		if (read_name(msg, len, &p, n2, sizeof n2) != 0) return -1;
		if (p + 20 > end) return -1;
		snprintf(out, size, "%s %s %lu %lu %lu %lu %lu", n1, n2, be32(msg + p), be32(msg + p + 4),
		         be32(msg + p + 8), be32(msg + p + 12), be32(msg + p + 16));
		return 0;
	case DNS_TXT:
	{
		int o = 0;
		while (p < end)
		{
			int l = msg[p++], i;
			if (p + l > end) return -1;
			if (o + 4 >= size) break;
			if (o) out[o++] = ' ';
			out[o++] = '"';
			for (i = 0; i < l && o < size - 3; i++)
			{
				unsigned char c = msg[p + i];
				if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
				else out[o++] = (c >= 32 && c < 127) ? (char)c : '?';
			}
			out[o++] = '"';
			p += l;
		}
		out[o] = '\0';
		return 0;
	}
	default:
	{
		/* unknown type: RFC 3597 form */
		int o = snprintf(out, size, "\\# %d ", rdlen), i;
		for (i = 0; i < rdlen && o < size - 3; i++)
			o += snprintf(out + o, size - o, "%02X", msg[p + i]);
		return 0;
	}
	}
}

int dns_parse_reply(const unsigned char* msg, int len, struct dns_reply* out)
{
	int pos = 12, i, total;

	memset(out, 0, sizeof *out);
	if (len < 12) return -1;
	out->id = be16(msg);
	out->flags = be16(msg + 2);
	out->rcode = out->flags & 0x0F;
	out->qdcount = be16(msg + 4);
	out->ancount = be16(msg + 6);
	out->nscount = be16(msg + 8);
	out->arcount = be16(msg + 10);

	for (i = 0; i < out->qdcount; i++)
	{
		char qn[256];
		if (read_name(msg, len, &pos, qn, sizeof qn) != 0 || pos + 4 > len) return -1;
		if (i == 0)
		{
			strcpy(out->qname, qn);
			out->qtype = be16(msg + pos);
		}
		pos += 4;
	}

	total = out->ancount + out->nscount + out->arcount;
	for (i = 0; i < total; i++)
	{
		struct dns_rr rr;
		int rdlen;
		memset(&rr, 0, sizeof rr);
		if (read_name(msg, len, &pos, rr.name, sizeof rr.name) != 0 || pos + 10 > len) return -1;
		rr.type = be16(msg + pos);
		rr.cls = be16(msg + pos + 2);
		rr.ttl = be32(msg + pos + 4);
		rdlen = be16(msg + pos + 8);
		pos += 10;
		if (pos + rdlen > len) return -1;
		rr.section = i < out->ancount ? 1 : i < out->ancount + out->nscount ? 2 : 3;
		if (rr.type == 41) { pos += rdlen; continue; }  /* EDNS OPT: not a record */
		if (format_rdata(msg, len, pos, rdlen, rr.type, rr.data, sizeof rr.data) != 0) return -1;
		pos += rdlen;
		if (out->count < DNS_MAX_RR)
			out->rr[out->count++] = rr;
	}
	return 0;
}

static const struct { const char* name; unsigned short type; } types[] = {
	{ "A", DNS_A }, { "NS", DNS_NS }, { "CNAME", DNS_CNAME }, { "SOA", DNS_SOA },
	{ "PTR", DNS_PTR }, { "MX", DNS_MX }, { "TXT", DNS_TXT }, { "AAAA", DNS_AAAA },
	{ "ANY", DNS_ANY }, { NULL, 0 }
};

const char* dns_type_name(unsigned short type)
{
	static char buf[12];
	int i;
	for (i = 0; types[i].name; i++)
		if (types[i].type == type) return types[i].name;
	snprintf(buf, sizeof buf, "TYPE%u", type);
	return buf;
}

int dns_type_from_name(const char* name)
{
	int i;
	for (i = 0; types[i].name; i++)
	{
		const char* a = types[i].name;
		const char* b = name;
		while (*a && *b && (*a == *b || *a == *b - 32)) { a++; b++; }
		if (!*a && !*b) return types[i].type;
	}
	return 0;
}

const char* dns_rcode_name(int rcode)
{
	static const char* names[] = { "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED" };
	static char buf[12];
	if (rcode >= 0 && rcode < 6) return names[rcode];
	snprintf(buf, sizeof buf, "RCODE%d", rcode);
	return buf;
}

int dns_reverse_name(const char* ip, char* out, int size)
{
	int a, b, c, d;
	char extra;
	if (sscanf(ip, "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) != 4) return 0;
	if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return 0;
	snprintf(out, size, "%d.%d.%d.%d.in-addr.arpa", d, c, b, a);
	return 1;
}
