/*
 * DNS messages for dig and nslookup: building queries and decoding replies.
 * Plain C with no Mac calls, so it can be tested on any computer.
 */
#ifndef MACSHELL_DNSMSG_H
#define MACSHELL_DNSMSG_H

#define DNS_MAX_RR 24

enum { DNS_A = 1, DNS_NS = 2, DNS_CNAME = 5, DNS_SOA = 6, DNS_PTR = 12, DNS_MX = 15,
       DNS_TXT = 16, DNS_AAAA = 28, DNS_ANY = 255 };

struct dns_rr
{
	char name[256];
	unsigned short type, cls;
	unsigned long ttl;
	char data[600];                 /* the record's data as text, as dig shows it (an SOA with two
	                                   255-character names and five numbers fits) */
	int section;                    /* 1 answer, 2 authority, 3 additional */
};

struct dns_reply
{
	unsigned short id, flags;
	int rcode, qdcount, ancount, nscount, arcount;
	char qname[256];
	unsigned short qtype;
	int count;                      /* records decoded (at most DNS_MAX_RR) */
	struct dns_rr rr[DNS_MAX_RR];
};

/* builds a query; returns its length, or -1 if the name is invalid */
int dns_build_query(unsigned char* buf, int size, unsigned short id,
                    const char* name, unsigned short type, int recurse);

/* decodes a reply; returns 0, or -1 if it is malformed */
int dns_parse_reply(const unsigned char* msg, int len, struct dns_reply* out);

const char* dns_type_name(unsigned short type);     /* "A", "MX", or "TYPE99" */
int dns_type_from_name(const char* name);           /* 0 if unknown */
const char* dns_rcode_name(int rcode);              /* "NOERROR", "NXDOMAIN"... */

/* "1.2.3.4" -> "4.3.2.1.in-addr.arpa"; returns 0 if not an IPv4 address */
int dns_reverse_name(const char* ip, char* out, int size);

#endif
