/*
 * Copyright (c) 2004, PADL Software Pty Ltd.
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
 * 3. Neither the name of PADL Software nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY PADL SOFTWARE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL PADL SOFTWARE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "krb5/gsskrb5_locl.h"

RCSID("$Id$");

static int
oid_prefix_equal(gss_OID oid_enc, gss_OID prefix_enc, unsigned *suffix)
{
    int ret;
    heim_oid oid;
    heim_oid prefix;
 
    *suffix = 0;

    ret = der_get_oid(oid_enc->elements, oid_enc->length,
		      &oid, NULL);
    if (ret) {
	return 0;
    }

    ret = der_get_oid(prefix_enc->elements, prefix_enc->length,
		      &prefix, NULL);
    if (ret) {
	der_free_oid(&oid);
	return 0;
    }

    ret = 0;

    if (oid.length - 1 == prefix.length) {
	*suffix = oid.components[oid.length - 1];
	oid.length--;
	ret = (der_heim_oid_cmp(&oid, &prefix) == 0);
	oid.length++;
    }

    der_free_oid(&oid);
    der_free_oid(&prefix);

    return ret;
}

static OM_uint32 inquire_sec_context_tkt_flags
           (OM_uint32 *minor_status,
            const gsskrb5_ctx context_handle,
            gss_buffer_set_t *data_set)
{
    OM_uint32 tkt_flags;
    unsigned char buf[4];
    gss_buffer_desc value;

    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);

    if (context_handle->ticket == NULL) {
	HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);
	*minor_status = EINVAL;
	return GSS_S_BAD_MECH;
    }

    tkt_flags = TicketFlags2int(context_handle->ticket->ticket.flags);
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);

    _gsskrb5_encode_om_uint32(tkt_flags, buf);
    value.length = sizeof(buf);
    value.value = buf;

    return gss_add_buffer_set_member(minor_status,
				     &value,
				     data_set);
}

static OM_uint32 inquire_sec_context_authz_data
           (OM_uint32 *minor_status,
            const gsskrb5_ctx context_handle,
            unsigned ad_type,
            gss_buffer_set_t *data_set)
{
    krb5_data data;
    gss_buffer_desc ad_data;
    OM_uint32 ret;

    *minor_status = 0;
    *data_set = GSS_C_NO_BUFFER_SET;

    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);
    if (context_handle->ticket == NULL) {
	HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);
	*minor_status = EINVAL;
	return GSS_S_FAILURE;
    }

    ret = krb5_ticket_get_authorization_data_type(_gsskrb5_context,
						  context_handle->ticket,
						  ad_type,
						  &data);
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);
    if (ret) {
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    ad_data.value = data.data;
    ad_data.length = data.length;

    ret = gss_add_buffer_set_member(minor_status,
				    &ad_data,
				    data_set);

    krb5_data_free(&data);

    return ret;
}

static OM_uint32 inquire_sec_context_has_updated_spnego
           (OM_uint32 *minor_status,
            const gsskrb5_ctx context_handle,
            gss_buffer_set_t *data_set)
{
    int is_updated = 0;

    *minor_status = 0;
    *data_set = GSS_C_NO_BUFFER_SET;

    /*
     * For Windows SPNEGO implementations, both the initiator and the
     * acceptor are assumed to have been updated if a "newer" [CLAR] or
     * different enctype is negotiated for use by the Kerberos GSS-API
     * mechanism.
     */
    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);
    _gsskrb5i_is_cfx(context_handle, &is_updated);
    if (is_updated == 0) {
	krb5_keyblock *acceptor_subkey;

	if (context_handle->more_flags & LOCAL)
	    acceptor_subkey = context_handle->auth_context->remote_subkey;
	else
	    acceptor_subkey = context_handle->auth_context->local_subkey;

	if (acceptor_subkey != NULL)
	    is_updated = (acceptor_subkey->keytype !=
			  context_handle->auth_context->keyblock->keytype);
    }
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);

    return is_updated ? GSS_S_COMPLETE : GSS_S_FAILURE;
}

/*
 *
 */

static OM_uint32
export_lucid_sec_context_v1(OM_uint32 *minor_status,
			    gsskrb5_ctx context_handle,
			    gss_buffer_set_t *data_set)
{
    krb5_storage *sp = NULL;
    OM_uint32 major_status = GSS_S_COMPLETE;
    krb5_error_code ret;
    krb5_keyblock *key = NULL;
    int32_t number;
    int is_cfx;
    krb5_data data;
    
    *minor_status = 0;

    GSSAPI_KRB5_INIT ();

    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);

    _gsskrb5i_is_cfx(context_handle, &is_cfx);

    sp = krb5_storage_emem();
    if (sp == NULL) {
	ret = ENOMEM;
	goto out;
    }

    ret = krb5_store_int32(sp, 1);
    if (ret) goto out;
    ret = krb5_store_int32(sp, (context_handle->more_flags & LOCAL) ? 1 : 0);
    if (ret) goto out;
    ret = krb5_store_int32(sp, context_handle->lifetime);
    if (ret) goto out;
    krb5_auth_con_getlocalseqnumber (_gsskrb5_context,
				     context_handle->auth_context,
				     &number);
    ret = krb5_store_uint32(sp, (uint32_t)0); /* store top half as zero */
    ret = krb5_store_uint32(sp, (uint32_t)number);
    krb5_auth_getremoteseqnumber (_gsskrb5_context,
				  context_handle->auth_context,
				  &number);
    ret = krb5_store_uint32(sp, (uint32_t)0); /* store top half as zero */
    ret = krb5_store_uint32(sp, (uint32_t)number);
    ret = krb5_store_int32(sp, (is_cfx) ? 1 : 0);
    if (ret) goto out;

    ret = _gsskrb5i_get_subkey(context_handle, &key);
    if (ret) goto out;

    if (is_cfx == 0) {
	int sign_alg, seal_alg;

	switch (key->keytype) {
	case ETYPE_DES_CBC_CRC:
	case ETYPE_DES_CBC_MD4:
	case ETYPE_DES_CBC_MD5:
	    sign_alg = 0;
	    seal_alg = 0;
	    break;
	case ETYPE_DES3_CBC_MD5:
	case ETYPE_DES3_CBC_SHA1:
	    sign_alg = 4;
	    seal_alg = 2;
	    break;
	case ETYPE_ARCFOUR_HMAC_MD5:
	case ETYPE_ARCFOUR_HMAC_MD5_56:
	    sign_alg = 17;
	    seal_alg = 16;
	    break;
	default:
	    sign_alg = -1;
	    seal_alg = -1;
	    break;
	}
	ret = krb5_store_int32(sp, sign_alg);
	if (ret) goto out;
	ret = krb5_store_int32(sp, seal_alg);
	if (ret) goto out;
	/* ctx_key */
	ret = krb5_store_keyblock(sp, *key);
	if (ret) goto out;
    } else {
	int subkey_p = (context_handle->more_flags & ACCEPTOR_SUBKEY) ? 1 : 0;

	/* have_acceptor_subkey */
	ret = krb5_store_int32(sp, subkey_p);
	if (ret) goto out;
	/* ctx_key */
	ret = krb5_store_keyblock(sp, *key);
	if (ret) goto out;
	/* acceptor_subkey */
	if (subkey_p) {
	    ret = krb5_store_keyblock(sp, *key);
	    if (ret) goto out;
	}
    }
    ret = krb5_storage_to_data(sp, &data);
    if (ret) goto out;

    {
	gss_buffer_desc ad_data;

	ad_data.value = data.data;
	ad_data.length = data.length;

	ret = gss_add_buffer_set_member(minor_status, &ad_data, data_set);
	krb5_data_free(&data);
	if (ret)
	    goto out;
    }

out:
    if (key)
	krb5_free_keyblock (_gsskrb5_context, key);
    if (sp)
	krb5_storage_free(sp);
    if (ret) {
	*minor_status = ret;
	major_status = GSS_S_FAILURE;
    }
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);
    return major_status;
}


OM_uint32 _gsskrb5_inquire_sec_context_by_oid
           (OM_uint32 *minor_status,
            const gss_ctx_id_t context_handle,
            const gss_OID desired_object,
            gss_buffer_set_t *data_set)
{
    const gsskrb5_ctx ctx = (const gsskrb5_ctx) context_handle;
    unsigned suffix;

    if (ctx == NULL) {
	*minor_status = EINVAL;
	return GSS_S_NO_CONTEXT;
    }

    if (gss_oid_equal(desired_object, GSS_KRB5_GET_TKT_FLAGS_X)) {
	return inquire_sec_context_tkt_flags(minor_status,
					     ctx,
					     data_set);
    } else if (gss_oid_equal(desired_object, GSS_C_PEER_HAS_UPDATED_SPNEGO)) {
	return inquire_sec_context_has_updated_spnego(minor_status,
						      ctx,
						      data_set);
    } else if (oid_prefix_equal(desired_object,
				GSS_KRB5_EXTRACT_AUTHZ_DATA_FROM_SEC_CONTEXT_X,
				&suffix)) {
	return inquire_sec_context_authz_data(minor_status,
					      ctx,
					      suffix,
					      data_set);
    } else if (oid_prefix_equal(desired_object,
				GSS_KRB5_EXPORT_LUCID_CONTEXT_X,
				&suffix)) {
	if (suffix == 1)
	    return export_lucid_sec_context_v1(minor_status,
					       ctx,
					       data_set);
	*minor_status = 0;
	return GSS_S_FAILURE;
    } else {
	*minor_status = 0;
	return GSS_S_FAILURE;
    }
}

