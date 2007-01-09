/*
 * Copyright (c) 2004 - 2007 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "hx_locl.h"
RCSID("$Id$");

#include <hxtool-commands.h>
#include <sl.h>
#include <parse_time.h>

static hx509_context context;

static int version_flag;
static int help_flag;

struct getargs args[] = {
    { "version", 0, arg_flag, &version_flag },
    { "help", 0, arg_flag, &help_flag }
};
int num_args = sizeof(args) / sizeof(args[0]);

static void
usage(int code)
{
    arg_printusage(args, num_args, NULL, "command");
    exit(code);
}

static void
lock_strings(hx509_lock lock, getarg_strings *pass)
{
    int i;
    for (i = 0; i < pass->num_strings; i++) {
	int ret = hx509_lock_command_string(lock, pass->strings[i]);
	if (ret)
	    errx(1, "hx509_lock_command_string: %s: %d", 
		 pass->strings[i], ret);
    }
}

static void
certs_strings(hx509_context context, const char *type, hx509_certs certs,
	      hx509_lock lock, const getarg_strings *s)
{
    int i, ret;

    for (i = 0; i < s->num_strings; i++) {
	ret = hx509_certs_append(context, certs, lock, s->strings[i]);
	if (ret)
	    hx509_err(context, ret, 1,
		      "hx509_certs_append: %s %s", type, s->strings[i]);
    }
}

int
cms_verify_sd(struct cms_verify_sd_options *opt, int argc, char **argv)
{
    hx509_verify_ctx ctx = NULL;
    heim_oid type;
    heim_octet_string c, co;
    hx509_certs store = NULL;
    hx509_certs signers = NULL;
    hx509_certs anchors = NULL;
    hx509_lock lock;
    int ret;

    size_t sz;
    void *p;

    if (opt->missing_revoke_flag)
	hx509_context_set_missing_revoke(context, 1);

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    ret = _hx509_map_file(argv[0], &p, &sz, NULL);
    if (ret)
	err(1, "map_file: %s: %d", argv[0], ret);

    ret = hx509_verify_init_ctx(context, &ctx);

    ret = hx509_certs_init(context, "MEMORY:cms-anchors", 0, NULL, &anchors);
    ret = hx509_certs_init(context, "MEMORY:cert-store", 0, NULL, &store);

    certs_strings(context, "anchors", anchors, lock, &opt->anchors_strings);
    certs_strings(context, "store", store, lock, &opt->certificate_strings);

    co.data = p;
    co.length = sz;

    if (opt->content_info_flag) {
	heim_octet_string uwco;
	heim_oid oid;

	ret = hx509_cms_unwrap_ContentInfo(&co, &oid, &uwco, NULL);
	if (ret)
	    errx(1, "hx509_cms_unwrap_ContentInfo: %d", ret);

	if (der_heim_oid_cmp(&oid, oid_id_pkcs7_signedData()) != 0)
	    errx(1, "Content is not SignedData");
	der_free_oid(&oid);

	co = uwco;
    }

    hx509_verify_attach_anchors(ctx, anchors);

    ret = hx509_cms_verify_signed(context, ctx, co.data, co.length,
				  store, &type, &c, &signers);
    if (co.data != p)
	der_free_octet_string(&co);
    if (ret)
	hx509_err(context, 1, ret, "hx509_cms_verify_signed");

    {
	char *str;
	der_print_heim_oid(&type, '.', &str);
	printf("type: %s\n", str);
	free(str);
	der_free_oid(&type);
    }
    printf("signers:\n");
    hx509_certs_iter(context, signers, hx509_ci_print_names, stdout);

    hx509_verify_destroy_ctx(ctx);

    hx509_certs_free(&store);
    hx509_certs_free(&signers);
    hx509_certs_free(&anchors);

    hx509_lock_free(lock);

    ret = _hx509_write_file(argv[1], c.data, c.length);
    if (ret)
	errx(1, "hx509_write_file: %d", ret);

    der_free_octet_string(&c);
    _hx509_unmap_file(p, sz);

    return 0;
}

int
cms_create_sd(struct cms_create_sd_options *opt, int argc, char **argv)
{
    const heim_oid *contentType;
    heim_octet_string o;
    hx509_query *q;
    hx509_lock lock;
    hx509_certs store, pool, anchors;
    hx509_cert cert;
    size_t sz;
    void *p;
    int ret;

    contentType = oid_id_pkcs7_data();

    if (argc < 2)
	errx(1, "argc < 2");

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    ret = hx509_certs_init(context, "MEMORY:cert-store", 0, NULL, &store);
    ret = hx509_certs_init(context, "MEMORY:cert-pool", 0, NULL, &pool);

    certs_strings(context, "store", store, lock, &opt->certificate_strings);
    certs_strings(context, "pool", pool, lock, &opt->pool_strings);

    if (opt->anchors_strings.num_strings) {
	ret = hx509_certs_init(context, "MEMORY:cert-anchors", 
			       0, NULL, &anchors);
	certs_strings(context, "anchors", anchors, lock, &opt->anchors_strings);
    } else
	anchors = NULL;

    ret = hx509_query_alloc(context, &q);
    if (ret)
	errx(1, "hx509_query_alloc: %d", ret);

    hx509_query_match_option(q, HX509_QUERY_OPTION_PRIVATE_KEY);
    hx509_query_match_option(q, HX509_QUERY_OPTION_KU_DIGITALSIGNATURE);
			     
    if (opt->signer_string)
	hx509_query_match_friendly_name(q, opt->signer_string);

    ret = hx509_certs_find(context, store, q, &cert);
    hx509_query_free(context, q);
    if (ret)
	errx(1, "hx509_certs_find: %d", ret);

    ret = _hx509_map_file(argv[0], &p, &sz, NULL);
    if (ret)
	err(1, "map_file: %s: %d", argv[0], ret);

    ret = hx509_cms_create_signed_1(context,
				    contentType,
				    p,
				    sz, 
				    NULL,
				    cert,
				    NULL,
				    anchors,
				    pool,
				    &o);
    if (ret)
	errx(1, "hx509_cms_create_signed: %d", ret);

    hx509_certs_free(&anchors);
    hx509_certs_free(&pool);
    hx509_cert_free(cert);
    hx509_certs_free(&store);
    _hx509_unmap_file(p, sz);
    hx509_lock_free(lock);

    if (opt->content_info_flag) {
	heim_octet_string wo;

	ret = hx509_cms_wrap_ContentInfo(oid_id_pkcs7_signedData(), &o, &wo);
	if (ret)
	    errx(1, "hx509_cms_wrap_ContentInfo: %d", ret);

	der_free_octet_string(&o);
	o = wo;
    }

    ret = _hx509_write_file(argv[1], o.data, o.length);
    if (ret)
	errx(1, "hx509_write_file: %d", ret);

    free(o.data);

    return 0;
}

int
cms_unenvelope(struct cms_unenvelope_options *opt, int argc, char **argv)
{
    heim_oid contentType = { 0, NULL };
    heim_octet_string o, co;
    hx509_certs certs;
    size_t sz;
    void *p;
    int ret;
    hx509_lock lock;

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    ret = _hx509_map_file(argv[0], &p, &sz, NULL);
    if (ret)
	err(1, "map_file: %s: %d", argv[0], ret);

    co.data = p;
    co.length = sz;

    if (opt->content_info_flag) {
	heim_octet_string uwco;
	heim_oid oid;

	ret = hx509_cms_unwrap_ContentInfo(&co, &oid, &uwco, NULL);
	if (ret)
	    errx(1, "hx509_cms_unwrap_ContentInfo: %d", ret);

	if (der_heim_oid_cmp(&oid, oid_id_pkcs7_envelopedData()) != 0)
	    errx(1, "Content is not SignedData");
	der_free_oid(&oid);

	co = uwco;
    }

    ret = hx509_certs_init(context, "MEMORY:cert-store", 0, NULL, &certs);
    if (ret)
	errx(1, "hx509_certs_init: MEMORY: %d", ret);

    certs_strings(context, "store", certs, lock, &opt->certificate_strings);

    ret = hx509_cms_unenvelope(context, certs, 0, co.data, co.length,
			       NULL, &contentType, &o);
    if (co.data != p)
	der_free_octet_string(&co);
    if (ret)
	hx509_err(context, 1, ret, "hx509_cms_unenvelope");

    _hx509_unmap_file(p, sz);
    hx509_lock_free(lock);
    hx509_certs_free(&certs);
    der_free_oid(&contentType);

    ret = _hx509_write_file(argv[1], o.data, o.length);
    if (ret)
	errx(1, "hx509_write_file: %d", ret);

    der_free_octet_string(&o);

    return 0;
}

int
cms_create_enveloped(struct cms_envelope_options *opt, int argc, char **argv)
{
    heim_octet_string o;
    const heim_oid *enctype = NULL;
    hx509_query *q;
    hx509_certs certs;
    hx509_cert cert;
    int ret;
    size_t sz;
    void *p;
    hx509_lock lock;

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    ret = _hx509_map_file(argv[0], &p, &sz, NULL);
    if (ret)
	err(1, "map_file: %s: %d", argv[0], ret);

    ret = hx509_certs_init(context, "MEMORY:cert-store", 0, NULL, &certs);

    certs_strings(context, "store", certs, lock, &opt->certificate_strings);

    if (opt->encryption_type_string) {
	enctype = hx509_crypto_enctype_by_name(opt->encryption_type_string);
	if (enctype == NULL)
	    errx(1, "encryption type: %s no found", 
		 opt->encryption_type_string);
    }

    ret = hx509_query_alloc(context, &q);
    if (ret)
	errx(1, "hx509_query_alloc: %d", ret);

    hx509_query_match_option(q, HX509_QUERY_OPTION_KU_ENCIPHERMENT);

    ret = hx509_certs_find(context, certs, q, &cert);
    hx509_query_free(context, q);
    if (ret)
	errx(1, "hx509_certs_find: %d", ret);

    ret = hx509_cms_envelope_1(context, cert, p, sz, enctype, 
			       oid_id_pkcs7_data(), &o);
    if (ret)
	errx(1, "hx509_cms_envelope_1: %d", ret);

    hx509_cert_free(cert);
    hx509_certs_free(&certs);
    _hx509_unmap_file(p, sz);

    if (opt->content_info_flag) {
	heim_octet_string wo;

	ret = hx509_cms_wrap_ContentInfo(oid_id_pkcs7_envelopedData(), &o, &wo);
	if (ret)
	    errx(1, "hx509_cms_wrap_ContentInfo: %d", ret);

	der_free_octet_string(&o);
	o = wo;
    }

    hx509_lock_free(lock);

    ret = _hx509_write_file(argv[1], o.data, o.length);
    if (ret)
	errx(1, "hx509_write_file: %d", ret);

    der_free_octet_string(&o);

    return 0;
}

static void
print_certificate(hx509_context hxcontext, hx509_cert cert, int verbose)
{
    hx509_name name;
    const char *fn;
    char *str;
    int ret;
    
    fn = hx509_cert_get_friendly_name(cert);
    if (fn)
	printf("    friendly name: %s\n", fn);
    printf("    private key: %s\n", 
	   _hx509_cert_private_key(cert) ? "yes" : "no");

    ret = hx509_cert_get_issuer(cert, &name);
    hx509_name_to_string(name, &str);
    hx509_name_free(&name);
    printf("    issuer:  \"%s\"\n", str);
    free(str);

    ret = hx509_cert_get_subject(cert, &name);
    hx509_name_to_string(name, &str);
    hx509_name_free(&name);
    printf("    subject: \"%s\"\n", str);
    free(str);

    {
	heim_integer serialNumber;

	hx509_cert_get_serialnumber(cert, &serialNumber);
	der_print_hex_heim_integer(&serialNumber, &str);
	der_free_heim_integer(&serialNumber);
	printf("    serial: %s\n", str);
	free(str);
    }

    printf("    keyusage: ");
    ret = hx509_cert_keyusage_print(hxcontext, cert, &str);
    if (ret == 0) {
	printf("%s\n", str);
	free(str);
    } else
	printf("no");

    if (verbose) {
	hx509_validate_ctx vctx;

	hx509_validate_ctx_init(hxcontext, &vctx);
	hx509_validate_ctx_set_print(vctx, hx509_print_stdout, stdout);
	hx509_validate_ctx_add_flags(vctx, HX509_VALIDATE_F_VALIDATE);
	hx509_validate_ctx_add_flags(vctx, HX509_VALIDATE_F_VERBOSE);
	
	hx509_validate_cert(hxcontext, vctx, cert);
    }
}


struct print_s {
    int counter;
    int verbose;
};

static int
print_f(hx509_context hxcontext, void *ctx, hx509_cert cert)
{
    struct print_s *s = ctx;
    
    printf("cert: %d\n", s->counter++);
    print_certificate(context, cert, s->verbose);

    return 0;
}

int
pcert_print(struct print_options *opt, int argc, char **argv)
{
    hx509_certs certs;
    hx509_lock lock;
    struct print_s s;

    s.counter = 0;
    s.verbose = opt->content_flag;

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    while(argc--) {
	int ret;
	ret = hx509_certs_init(context, argv[0], 0, lock, &certs);
	if (ret)
	    hx509_err(context, 1, ret, "hx509_certs_init");
	if (opt->info_flag)
	    hx509_certs_info(context, certs, NULL, NULL);
	hx509_certs_iter(context, certs, print_f, &s);
	hx509_certs_free(&certs);
	argv++;
    }

    hx509_lock_free(lock);

    return 0;
}


static int
validate_f(hx509_context hxcontext, void *ctx, hx509_cert c)
{
    hx509_validate_cert(hxcontext, ctx, c);
    return 0;
}

int
pcert_validate(struct validate_options *opt, int argc, char **argv)
{
    hx509_validate_ctx ctx;
    hx509_certs certs;
    hx509_lock lock;

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    hx509_validate_ctx_init(context, &ctx);
    hx509_validate_ctx_set_print(ctx, hx509_print_stdout, stdout);
    hx509_validate_ctx_add_flags(ctx, HX509_VALIDATE_F_VALIDATE);

    while(argc--) {
	int ret;
	ret = hx509_certs_init(context, argv[0], 0, lock, &certs);
	if (ret)
	    errx(1, "hx509_certs_init: %d", ret);
	hx509_certs_iter(context, certs, validate_f, ctx);
	hx509_certs_free(&certs);
	argv++;
    }
    hx509_validate_ctx_free(ctx);

    hx509_lock_free(lock);

    return 0;
}

int
certificate_copy(struct certificate_copy_options *opt, int argc, char **argv)
{
    hx509_certs certs;
    hx509_lock lock;
    int ret;

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->in_pass_strings);

    ret = hx509_certs_init(context, argv[argc - 1], 
			   HX509_CERTS_CREATE, lock, &certs);
    if (ret)
	hx509_err(context, 1, ret, "hx509_certs_init");

    while(argc-- > 1) {
	int ret;
	ret = hx509_certs_append(context, certs, lock, argv[0]);
	if (ret)
	    hx509_err(context, 1, ret, "hx509_certs_append");
	argv++;
    }

    ret = hx509_certs_store(context, certs, 0, NULL);
	if (ret)
	    hx509_err(context, 1, ret, "hx509_certs_store");

    hx509_certs_free(&certs);

    return 0;
}

struct verify {
    hx509_verify_ctx ctx;
    hx509_certs chain;
};

static int
verify_f(hx509_context hxcontext, void *ctx, hx509_cert c)
{
    struct verify *v = ctx;
    int ret;

    ret = hx509_verify_path(hxcontext, v->ctx, c, v->chain);
    if (ret) {
	char *s = hx509_get_error_string(hxcontext, ret);
	printf("verify_path: %s: %d\n", s, ret);
	free(s);
    } else
	printf("path ok\n");

    return ret;
}

int
pcert_verify(struct verify_options *opt, int argc, char **argv)
{
    hx509_certs anchors, chain, certs;
    hx509_revoke_ctx revoke_ctx;
    hx509_verify_ctx ctx;
    struct verify v;
    int ret;

    if (opt->missing_revoke_flag)
	hx509_context_set_missing_revoke(context, 1);

    ret = hx509_verify_init_ctx(context, &ctx);
    ret = hx509_certs_init(context, "MEMORY:anchors", 0, NULL, &anchors);
    ret = hx509_certs_init(context, "MEMORY:chain", 0, NULL, &chain);
    ret = hx509_certs_init(context, "MEMORY:certs", 0, NULL, &certs);

    if (opt->allow_proxy_certificate_flag)
	hx509_verify_set_proxy_certificate(ctx, 1);

    if (opt->time_string) {
	const char *p;
	struct tm tm;
	time_t t;

	memset(&tm, 0, sizeof(tm));

	p = strptime (opt->time_string, "%Y-%m-%d", &tm);
	if (p == NULL)
	    errx(1, "Failed to parse time %s, need to be on format %%Y-%%m-%%d",
		 opt->time_string);
	
	t = tm2time (tm, 0);

	hx509_verify_set_time(ctx, t);
    }

    ret = hx509_revoke_init(context, &revoke_ctx);
    if (ret)
	errx(1, "hx509_revoke_init: %d", ret);

    while(argc--) {
	char *s = *argv++;

	if (strncmp(s, "chain:", 6) == 0) {
	    s += 6;

	    ret = hx509_certs_append(context, chain, NULL, s);
	    if (ret)
		errx(1, "hx509_certs_append: chain: %s: %d", s, ret);

	} else if (strncmp(s, "anchor:", 7) == 0) {
	    s += 7;

	    ret = hx509_certs_append(context, anchors, NULL, s);
	    if (ret)
		errx(1, "hx509_certs_append: anchor: %s: %d", s, ret);

	} else if (strncmp(s, "cert:", 5) == 0) {
	    s += 5;

	    ret = hx509_certs_append(context, certs, NULL, s);
	    if (ret)
		hx509_err(context, 1, ret, "hx509_certs_append: certs: %s: %d", 
			  s, ret);

	} else if (strncmp(s, "crl:", 4) == 0) {
	    s += 4;

	    ret = hx509_revoke_add_crl(context, revoke_ctx, s);
	    if (ret)
		errx(1, "hx509_revoke_add_crl: %s: %d", s, ret);

	} else if (strncmp(s, "ocsp:", 4) == 0) {
	    s += 5;

	    ret = hx509_revoke_add_ocsp(context, revoke_ctx, s);
	    if (ret)
		errx(1, "hx509_revoke_add_ocsp: %s: %d", s, ret);

	} else {
	    errx(1, "unknown option to verify: `%s'\n", s);
	}
    }

    hx509_verify_attach_anchors(ctx, anchors);
    hx509_verify_attach_revoke(ctx, revoke_ctx);

    v.ctx = ctx;
    v.chain = chain;

    ret = hx509_certs_iter(context, certs, verify_f, &v);

    hx509_verify_destroy_ctx(ctx);

    hx509_certs_free(&certs);
    hx509_certs_free(&chain);
    hx509_certs_free(&anchors);

    hx509_revoke_free(&revoke_ctx);

    return ret;
}

int
query(struct query_options *opt, int argc, char **argv)
{
    hx509_lock lock;
    hx509_query *q;
    hx509_certs certs;
    hx509_cert c;
    int ret;

    ret = hx509_query_alloc(context, &q);
    if (ret)
	errx(1, "hx509_query_alloc: %d", ret);

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    ret = hx509_certs_init(context, "MEMORY:cert-store", 0, NULL, &certs);

    while (argc > 0) {

	ret = hx509_certs_append(context, certs, lock, argv[0]);
	if (ret)
	    errx(1, "hx509_certs_append: %s: %d", argv[0], ret);

	argc--;
	argv++;
    }

    if (opt->friendlyname_string)
	hx509_query_match_friendly_name(q, opt->friendlyname_string);

    if (opt->private_key_flag)
	hx509_query_match_option(q, HX509_QUERY_OPTION_PRIVATE_KEY);

    if (opt->keyEncipherment_flag)
	hx509_query_match_option(q, HX509_QUERY_OPTION_KU_ENCIPHERMENT);

    if (opt->digitalSignature_flag)
	hx509_query_match_option(q, HX509_QUERY_OPTION_KU_DIGITALSIGNATURE);

    ret = hx509_certs_find(context, certs, q, &c);
    hx509_query_free(context, q);
    if (ret)
	printf("no match found (%d)\n", ret);
    else {
	printf("match found\n");
	if (opt->print_flag)
	    print_certificate(context, c, 0);
    }

    hx509_cert_free(c);
    hx509_certs_free(&certs);

    hx509_lock_free(lock);

    return ret;
}

int
ocsp_fetch(struct ocsp_fetch_options *opt, int argc, char **argv)
{
    hx509_certs reqcerts, pool;
    heim_octet_string req, nonce_data, *nonce = &nonce_data;
    hx509_lock lock;
    int i, ret;
    char *file;
    const char *url = "/";

    memset(&nonce, 0, sizeof(nonce));

    hx509_lock_init(context, &lock);
    lock_strings(lock, &opt->pass_strings);

    /* no nonce */
    if (!opt->nonce_flag)
	nonce = NULL;

    if (opt->url_path_string)
	url = opt->url_path_string;

    ret = hx509_certs_init(context, "MEMORY:ocsp-pool", 0, NULL, &pool);

    certs_strings(context, "ocsp-pool", pool, lock, &opt->pool_strings);

    file = argv[0];

    ret = hx509_certs_init(context, "MEMORY:ocsp-req", 0, NULL, &reqcerts);

    for (i = 1; i < argc; i++) {
	ret = hx509_certs_append(context, reqcerts, lock, argv[i]);
	if (ret)
	    errx(1, "hx509_certs_append: req: %s: %d", argv[i], ret);
    }

    ret = hx509_ocsp_request(context, reqcerts, pool, NULL, NULL, &req, nonce);
    if (ret)
	errx(1, "hx509_ocsp_request: req: %d", ret);
	
    {
	FILE *f;

	f = fopen(file, "w");
	if (f == NULL)
	    abort();

	fprintf(f, 
		"POST %s HTTP/1.0\r\n"
		"Content-Type: application/ocsp-request\r\n"
		"Content-Length: %ld\r\n"
		"\r\n",
		url,
		(unsigned long)req.length);
	fwrite(req.data, req.length, 1, f);
	fclose(f);
    }

    if (nonce)
	der_free_octet_string(nonce);

    hx509_certs_free(&reqcerts);
    hx509_certs_free(&pool);

    return 0;
}

int
ocsp_print(struct ocsp_print_options *opt, int argc, char **argv)
{
    hx509_revoke_ocsp_print(context, argv[0], stdout);
    return 0;
}

static int
read_private_key(const char *fn, hx509_private_key *key)
{
    hx509_private_key *keys;
    hx509_certs certs;
    int ret;
    
    *key = NULL;

    ret = hx509_certs_init(context, fn, 0, NULL, &certs);
    if (ret)
	hx509_err(context, ret, 1, "hx509_certs_init: %s", fn);

    ret = _hx509_certs_keys_get(context, certs, &keys);
    hx509_certs_free(&certs);
    if (ret)
	hx509_err(context, ret, 1, "hx509_certs_keys_get");
    if (keys[0] == NULL)
	errx(1, "no keys in key store: %s", fn);

    *key = _hx509_private_key_ref(keys[0]);
    _hx509_certs_keys_free(context, keys);

    return 0;
}

static void
get_key(const char *fn, const char *type, int optbits, 
	hx509_private_key *signer)
{
    int ret;

    if (type) {
	BIGNUM *e;
	RSA *rsa;
	unsigned char *p0, *p;
	size_t len;
	int bits = 1024;

	if (fn == NULL)
	    errx(1, "no key argument, don't know here to store key");
	
	if (strcasecmp(type, "rsa") != 0)
	    errx(1, "can only handle rsa keys for now");
	    
	e = BN_new();
	BN_set_word(e, 0x10001);

	if (optbits)
	    bits = optbits;

	rsa = RSA_new();
	if(rsa == NULL)
	    errx(1, "RSA_new failed");

	ret = RSA_generate_key_ex(rsa, bits, e, NULL);
	if(ret != 1)
	    errx(1, "RSA_new failed");

	BN_free(e);

	len = i2d_RSAPrivateKey(rsa, NULL);

	p0 = p = malloc(len);
	if (p == NULL)
	    errx(1, "out of memory");
	
	i2d_RSAPrivateKey(rsa, &p);

	rk_dumpdata(fn, p0, len);
	memset(p0, 0, len);
	free(p0);
	
	RSA_free(rsa);

    } else if (fn == NULL)
	err(1, "no private key");

    ret = read_private_key(fn, signer);
    if (ret)
	err(1, "read_private_key");
}

int
request_create(struct request_create_options *opt, int argc, char **argv)
{
    heim_octet_string request;
    hx509_request req;
    int ret, i;
    hx509_private_key signer;
    SubjectPublicKeyInfo key;
    const char *outfile = argv[0];

    memset(&key, 0, sizeof(key));

    get_key(opt->key_string, 
	    opt->generate_key_string,
	    opt->key_bits_integer,
	    &signer);
    
    _hx509_request_init(context, &req);

    if (opt->subject_string) {
	hx509_name name = NULL;

	ret = hx509_parse_name(context, opt->subject_string, &name);
	if (ret)
	    errx(1, "hx509_parse_name: %d\n", ret);
	_hx509_request_set_name(context, req, name);

	if (opt->verbose_flag) {
	    char *s;
	    hx509_name_to_string(name, &s);
	    printf("%s\n", s);
	}
	hx509_name_free(&name);
    }

    for (i = 0; i < opt->email_strings.num_strings; i++) {
	ret = _hx509_request_add_email(context, req, 
				       opt->email_strings.strings[i]);
    }

    for (i = 0; i < opt->dnsname_strings.num_strings; i++) {
	ret = _hx509_request_add_dns_name(context, req, 
					  opt->dnsname_strings.strings[i]);
    }


    ret = _hx509_private_key2SPKI(context, signer, &key);
    if (ret)
	errx(1, "_hx509_private_key2SPKI: %d\n", ret);

    ret = _hx509_request_set_SubjectPublicKeyInfo(context,
						  req,
						  &key);
    free_SubjectPublicKeyInfo(&key);
    if (ret)
	hx509_err(context, ret, 1, "_hx509_request_set_SubjectPublicKeyInfo");

    ret = _hx509_request_to_pkcs10(context,
				   req,
				   signer,
				   &request);
    if (ret)
	hx509_err(context, ret, 1, "_hx509_request_to_pkcs10");

    _hx509_private_key_free(&signer);
    _hx509_request_free(&req);

    if (ret == 0)
	rk_dumpdata(outfile, request.data, request.length);
    der_free_octet_string(&request);

    return 0;
}

int
pkcs10_print(struct pkcs10_print_options *opt, int argc, char **argv)
{
    size_t size, length;
    int ret, i;
    void *p;

    printf("pkcs10 print\n");

    for (i = 0; i < argc; i++) {
	CertificationRequest req;
	CertificationRequestInfo *rinfo;

	ret = _hx509_map_file(argv[i], &p, &length, NULL);
	if (ret)
	    err(1, "map_file: %s: %d", argv[i], ret);

	ret = decode_CertificationRequest(p, length, &req, &size);
	_hx509_unmap_file(p, length);
	if (ret)
	    errx(1, "failed to parse file %s: %d", argv[i], ret);

	rinfo = &req.certificationRequestInfo;

	{
	    char *subject;
	    hx509_name n;

	    ret = _hx509_name_from_Name(&rinfo->subject, &n);
	    if (ret)
		abort();
	    
	    ret = hx509_name_to_string(n, &subject);
	    hx509_name_free(&n);
	    if (ret)
		abort();
	    
	    printf("name: %s\n", subject);
	    free(subject);
	}

	if (rinfo->attributes && rinfo->attributes->len) {
	    int j;

	    printf("Attributes:\n");

	    for (j = 0; j < rinfo->attributes->len; j++) {
		char *str;
		hx509_oid_sprint(&rinfo->attributes->val[j].type, &str);
		printf("\toid: %s\n", str);
		free(str);
	    }
	}

	free_CertificationRequest(&req);
    }

    return 0;
}

int
info(void *opt, int argc, char **argv)
{

    ENGINE_add_conf_module();

    {
	const RSA_METHOD *m = RSA_get_default_method();
	if (m != NULL)
	    printf("rsa: %s\n", m->name);
    }
    {
	const DH_METHOD *m = DH_get_default_method();
	if (m != NULL)
	    printf("dh: %s\n", m->name);
    }

    return 0;
}

int
random_data(void *opt, int argc, char **argv)
{
    void *ptr;
    int len, ret;

    len = parse_bytes(argv[0], "byte");
    if (len <= 0) {
	fprintf(stderr, "bad argument to random-data\n");
	return 1;
    }

    ptr = malloc(len);
    if (ptr == NULL) {
	fprintf(stderr, "out of memory\n");
	return 1;
    }

    ret = RAND_bytes(ptr, len);
    if (ret != 1) {
	free(ptr);
	fprintf(stderr, "did not get cryptographic strong random\n");
	return 1;
    }

    fwrite(ptr, len, 1, stdout);
    fflush(stdout);

    free(ptr);

    return 0;
}

int
crypto_available(struct crypto_available_options *opt, int argc, char **argv)
{
    AlgorithmIdentifier *val;
    unsigned int len, i;
    int ret, type;

    if (opt->type_string) {
	if (strcmp(opt->type_string, "all") == 0)
	    type = HX509_SELECT_ALL;
	else if (strcmp(opt->type_string, "digest") == 0)
	    type = HX509_SELECT_DIGEST;
	else if (strcmp(opt->type_string, "public-sig") == 0)
	    type = HX509_SELECT_PUBLIC_SIG;
	else
	    errx(1, "unknown type: %s", opt->type_string);
    } else
	type = HX509_SELECT_ALL;

    ret = hx509_crypto_available(context, type, NULL, &val, &len);
    if (ret)
	errx(1, "hx509_crypto_available");

    for (i = 0; i < len; i++) {
	char *s;
	der_print_heim_oid (&val[i].algorithm, '.', &s);
	printf("%s\n", s);
	free(s);
    }

    hx509_crypto_free_algs(val, len);

    return 0;
}

int
crypto_select(struct crypto_select_options *opt, int argc, char **argv)
{
    hx509_peer_info peer = NULL;
    AlgorithmIdentifier selected;
    int ret, type;
    char *s;

    if (opt->type_string) {
	if (strcmp(opt->type_string, "digest") == 0)
	    type = HX509_SELECT_DIGEST;
	else if (strcmp(opt->type_string, "public-sig") == 0)
	    type = HX509_SELECT_PUBLIC_SIG;
	else
	    errx(1, "unknown type: %s", opt->type_string);
    } else
	type = HX509_SELECT_DIGEST;

    if (opt->peer_cmstype_strings.num_strings) {
	AlgorithmIdentifier *val;
	size_t i;

	ret = hx509_peer_info_alloc(context, &peer);
	if (ret)
	    errx(1, "hx509_peer_info_alloc");

	val = calloc(opt->peer_cmstype_strings.num_strings, sizeof(*val));
	if (val == NULL)
	    err(1, "malloc");

	for (i = 0; i < opt->peer_cmstype_strings.num_strings; i++) {
	    ret = der_parse_heim_oid (opt->peer_cmstype_strings.strings[i],
				      " .", &val[i].algorithm);
	    if  (ret)
		errx(1, "der_parse_heim_oid failed on: %s", 
		     opt->peer_cmstype_strings.strings[i]);
	}
	    
	ret = hx509_peer_info_set_cms_algs(context, peer, val, 
					   opt->peer_cmstype_strings.num_strings);
	if (ret)
	    errx(1, "hx509_peer_info_set_cms_algs");

    }

    ret = hx509_crypto_select(context, type, NULL, peer, &selected);
    if (ret)
	errx(1, "hx509_crypto_available");

    der_print_heim_oid (&selected.algorithm, '.', &s);
    printf("%s\n", s);
    free(s);
    free_AlgorithmIdentifier(&selected);

    return 0;
}

int
hxtool_hex(struct hex_options *opt, int argc, char **argv)
{

    if (opt->decode_flag) {
	char buf[1024], buf2[1024], *p;
	ssize_t len;

	while(fgets(buf, sizeof(buf), stdin) != NULL) {
	    buf[strcspn(buf, "\r\n")] = '\0';
	    p = buf;
	    while(isspace(*(unsigned char *)p))
		p++;
	    len = hex_decode(p, buf2, strlen(p));
	    if (len < 0)
		errx(1, "hex_decode failed");
	    if (fwrite(buf2, 1, len, stdout) != len)
		errx(1, "fwrite failed");
	}
    } else {
	char buf[28], *p;
	size_t len;

	while((len = fread(buf, 1, sizeof(buf), stdin)) != 0) {
	    len = hex_encode(buf, len, &p);
	    fprintf(stdout, "%s\n", p);
	    free(p);
	}
    }
    return 0;
}

static int
eval_types(hx509_context context, 
	   hx509_ca_tbs tbs,
	   const struct certificate_sign_options *opt)
{
    int pkinit = 0;
    int i, ret;

    for (i = 0; i < opt->type_strings.num_strings; i++) {
	const char *type = opt->type_strings.strings[i];
	
	if (strcmp(type, "https-server") == 0) {
	    ret = hx509_ca_tbs_add_eku(context, tbs, 
				       oid_id_pkix_kp_serverAuth());
	    if (ret)
		hx509_err(context, ret, 1, "hx509_ca_tbs_add_eku");
	} else if (strcmp(type, "https-client") == 0) {
	    ret = hx509_ca_tbs_add_eku(context, tbs, 
				       oid_id_pkix_kp_clientAuth());
	    if (ret)
		hx509_err(context, ret, 1, "hx509_ca_tbs_add_eku");
	} else if (strcmp(type, "pkinit-kdc") == 0) {
	    pkinit++;
	    ret = hx509_ca_tbs_add_eku(context, tbs, 
				       oid_id_pkkdcekuoid());
	    if (ret)
		hx509_err(context, ret, 1, "hx509_ca_tbs_add_eku");
	} else if (strcmp(type, "pkinit-client") == 0) {
	    pkinit++;
	    ret = hx509_ca_tbs_add_eku(context, tbs, 
				       oid_id_pkekuoid());
	    if (ret)
		hx509_err(context, ret, 1, "hx509_ca_tbs_add_eku");

	} else
	    errx(1, "unknown type %s", type);
    }

    if (pkinit > 1)
	errx(1, "More the one PK-INIT type given");

    if (opt->pk_init_principal_string) {
	if (!pkinit)
	    errx(1, "pk-init principal given but no pk-init oid");

	ret = hx509_ca_tbs_add_san_pkinit(context, tbs,
					  opt->pk_init_principal_string);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_tbs_add_san_pkinit");
    }
    
    for (i = 0; i < opt->hostname_strings.num_strings; i++) {
	const char *hostname = opt->hostname_strings.strings[i];

	ret = hx509_ca_tbs_add_san_hostname(context, tbs, hostname);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_tbs_add_san_hostname");
    }

    for (i = 0; i < opt->email_strings.num_strings; i++) {
	const char *email = opt->email_strings.strings[i];

	ret = hx509_ca_tbs_add_san_rfc822name(context, tbs, email);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_tbs_add_san_hostname");
    }

    return 0;
}

int
hxtool_ca(struct certificate_sign_options *opt, int argc, char **argv)
{
    int ret;
    hx509_ca_tbs tbs;
    hx509_certs cacerts = NULL;
    hx509_cert signer = NULL, cert = NULL;
    hx509_private_key private_key = NULL;
    hx509_private_key cert_key = NULL;
    hx509_name subject = NULL;
    SubjectPublicKeyInfo spki;
    int delta = 0;

    memset(&spki, 0, sizeof(spki));

    if (opt->ca_certificate_string == NULL && !opt->self_signed_flag)
	errx(1, "--ca-certificate argument missing (not using --self-signed)");
    if (opt->ca_private_key_string == NULL && opt->generate_key_string == NULL && opt->self_signed_flag)
	errx(1, "--ca-private-key argument missing (using --self-signed)");
    if (opt->certificate_string == NULL)
	errx(1, "--certificate argument missing");
    if (opt->req_string == NULL && opt->generate_key_string == NULL) {
	if (!opt->self_signed_flag)
	    errx(1, "--req argument missing");
    } else if (opt->req_string && opt->generate_key_string) {
	    errx(1, "can't do both --req and --generate-key");
    } else {
	if (opt->ca_private_key_string)
	    errx(1, "both --req and --ca-private-key used");
    }

    if (opt->lifetime_string) {
	delta = parse_time(opt->lifetime_string, "day");
	if (delta < 0)
	    errx(1, "Invalid lifetime: %s", opt->lifetime_string);
    }

    if (opt->ca_certificate_string) {
	hx509_query *q;

	ret = hx509_certs_init(context, opt->ca_certificate_string, 0,
			       NULL, &cacerts);
	if (ret)
	    hx509_err(context, ret, 1,
		      "hx509_certs_init: %s", opt->ca_certificate_string);

	ret = hx509_query_alloc(context, &q);
	if (ret)
	    errx(1, "hx509_query_alloc: %d", ret);

	hx509_query_match_option(q, HX509_QUERY_OPTION_PRIVATE_KEY);
	if (!opt->issue_proxy_flag)
	    hx509_query_match_option(q, HX509_QUERY_OPTION_KU_KEYCERTSIGN);

	ret = hx509_certs_find(context, cacerts, q, &signer);
	hx509_query_free(context, q);
	if (ret)
	    hx509_err(context, ret, 1, "no CA certificate found");
    } else if (opt->self_signed_flag) {
	if (opt->generate_key_string == NULL
	    && opt->ca_private_key_string == NULL)
	    errx(1, "no signing private key");
    } else
	errx(1, "missing ca key");

    if (opt->ca_private_key_string) {

	ret = read_private_key(opt->ca_private_key_string, &private_key);
	if (ret)
	    err(1, "read_private_key");

	ret = _hx509_private_key2SPKI(context, private_key, &spki);
	if (ret)
	    errx(1, "_hx509_private_key2SPKI: %d\n", ret);

	if (opt->self_signed_flag)
	    cert_key = private_key;

    }

    if (opt->req_string) {
	CertificationRequest req;
	CertificationRequestInfo *rinfo;
	void *p;
	size_t len, size;

	ret = _hx509_map_file(opt->req_string, &p, &len, NULL);
	if (ret)
	    err(1, "map_file: %s: %d", opt->req_string, ret);

	ret = decode_CertificationRequest(p, len, &req, &size);
	_hx509_unmap_file(p, len);
	if (ret)
	    errx(1, "failed to parse req file %s: %d", opt->req_string, ret);

	rinfo = &req.certificationRequestInfo;

	ret = _hx509_name_from_Name(&rinfo->subject, &subject);
	if (ret)
	    errx(1, "_hx509_name_from_Name %d", ret);

	ret = copy_SubjectPublicKeyInfo(&rinfo->subjectPKInfo, &spki);
	if (ret)
	    errx(1, "copy_SubjectPublicKeyInfo: %d", ret);

	free_CertificationRequest(&req);
    }

    if (opt->generate_key_string) {
	
	ret = _hx509_generate_private_key(context, 
					  oid_id_pkcs1_rsaEncryption(),
					  &cert_key);
	if (ret)
	    hx509_err(context, ret, 1, "generate private key");
	
	ret = _hx509_private_key2SPKI(context, cert_key, &spki);
	if (ret)
	    errx(1, "_hx509_private_key2SPKI: %d\n", ret);

	if (opt->self_signed_flag)
	    private_key = cert_key;
    }

    if (opt->subject_string) {
	if (subject)
	    hx509_name_free(&subject);
	ret = hx509_parse_name(context, opt->subject_string, &subject);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_parse_name: %d\n", ret);
    }

    if (opt->issue_proxy_flag) {
	if (subject)
	    hx509_name_free(&subject);
    } else {
	if (subject == NULL)
	    errx(1, "no subject given");
    }

    /*
     *
     */

    ret = hx509_ca_tbs_init(context, &tbs);
    if (ret)
	hx509_err(context, ret, 1, "hx509_ca_tbs_init");
	
    if (opt->serial_number_string) {
	heim_integer serialNumber;

	der_parse_hex_heim_integer(opt->serial_number_string,
				   &serialNumber);
	ret = hx509_ca_tbs_set_serialnumber(context, tbs, &serialNumber);
	der_free_heim_integer(&serialNumber);
    }

    ret = hx509_ca_tbs_set_spki(context, tbs, &spki);
    if (ret)
	hx509_err(context, ret, 1, "hx509_ca_tbs_set_spki");

    if (subject) {
	ret = hx509_ca_tbs_set_subject(context, tbs, subject);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_tbs_set_subject");
    }

    eval_types(context, tbs, opt);

    if (opt->issue_ca_flag) {
	ret = hx509_ca_tbs_set_ca(context, tbs, opt->path_length_integer);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_tbs_set_ca");
    }
    if (opt->issue_proxy_flag) {
	ret = hx509_ca_tbs_set_proxy(context, tbs, opt->path_length_integer);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_tbs_set_proxy");
    }

    if (delta) {
	ret = hx509_ca_tbs_set_notAfter_lifetime(context, tbs, delta);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_tbs_set_notAfter_lifetime");
    }	

    if (opt->self_signed_flag) {
	ret = hx509_ca_sign_self(context, tbs, private_key, &cert);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_sign_self");
    } else {
	ret = hx509_ca_sign(context, tbs, signer, &cert);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_ca_sign");
    }

    if (cert_key) {
	ret = _hx509_cert_assign_key(cert, cert_key);
	if (ret)
	    hx509_err(context, ret, 1, "_hx509_cert_assign_key");
    }	    

    {
	hx509_certs certs;

	ret = hx509_certs_init(context, opt->certificate_string, 
			       HX509_CERTS_CREATE, NULL, &certs);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_certs_init");

	ret = hx509_certs_add(context, certs, cert);
	if (ret)
	    hx509_err(context, ret, 1, "hx509_certs_add");

	ret = hx509_certs_store(context, certs, 0, NULL);
	if (ret)
	    hx509_err(context, 1, ret, "hx509_certs_store");

	hx509_certs_free(&certs);
    }

    return 0;
}


int
help(void *opt, int argc, char **argv)
{
    sl_slc_help(commands, argc, argv);
    return 0;
}

int
main(int argc, char **argv)
{
    int ret, optidx = 0;

    setprogname (argv[0]);

    if(getarg(args, num_args, argc, argv, &optidx))
	usage(1);
    if(help_flag)
	usage(0);
    if(version_flag) {
	print_version(NULL);
	exit(0);
    }
    argv += optidx;
    argc -= optidx;

    if (argc == 0)
	usage(1);

    ret = hx509_context_init(&context);
    if (ret)
	errx(1, "hx509_context_init failed with %d", ret);

    ret = sl_command(commands, argc, argv);
    if(ret == -1)
	warnx ("unrecognized command: %s", argv[0]);

    hx509_context_free(&context);

    return ret;
}
