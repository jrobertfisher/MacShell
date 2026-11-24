/* Laptop-side harness for dnsmsg.c: "build <name> <type>" writes a query to
   stdout; "parse" reads a reply on stdin and prints its records. */
#include "../../dnsmsg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	unsigned char buf[4096];
	if (argc == 4 && strcmp(argv[1], "build") == 0)
	{
		int n = dns_build_query(buf, sizeof buf, 0x4242, argv[2], dns_type_from_name(argv[3]), 1);
		if (n < 0) return 1;
		fwrite(buf, 1, n, stdout);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "parse") == 0)
	{
		static struct dns_reply r;
		int n = fread(buf, 1, sizeof buf, stdin), i;
		if (dns_parse_reply(buf, n, &r) != 0) { printf("MALFORMED\n"); return 1; }
		printf("status %s q %s %s\n", dns_rcode_name(r.rcode), r.qname, dns_type_name(r.qtype));
		for (i = 0; i < r.count; i++)
			printf("%d %s %s %s\n", r.rr[i].section, r.rr[i].name, dns_type_name(r.rr[i].type), r.rr[i].data);
		return 0;
	}
	return 2;
}
