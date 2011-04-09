/*
 * Copyright (c) 2011, PADL Software Pty Ltd.
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

#include "mech_locl.h"

static gss_buffer_desc localLoginUserAttr = {
    sizeof("local-login-user"),
    "local-login-user"
};

gss_buffer_t GSSAPI_LIB_VARIABLE GSS_C_ATTR_LOCAL_LOGIN_USER = &localLoginUserAttr;

static OM_uint32
mech_authorize_localname(OM_uint32 *minor_status,
	                 const struct _gss_name *name,
	                 const struct _gss_name *user,
	                 int *user_ok)
{
    OM_uint32 major_status = GSS_S_UNAVAILABLE;
    struct _gss_mechanism_name *mn;

    *user_ok = 0;

    HEIM_SLIST_FOREACH(mn, &name->gn_mn, gmn_link) {
        gssapi_mech_interface m = mn->gmn_mech;

        if (!m->gm_authorize_localname)
            continue;

        major_status = m->gm_authorize_localname(minor_status,
                                                 mn->gmn_name,
                                                 &user->gn_value,
                                                 &user->gn_type,
                                                 user_ok);
        if (GSS_ERROR(major_status) || *user_ok)
            break;
    }

    return major_status;
}

/*
 * Naming extensions based local login authorization.
 */
static OM_uint32
attr_authorize_localname(OM_uint32 *minor_status,
	                 const struct _gss_name *name,
	                 const struct _gss_name *user,
	                 int *user_ok)
{
    OM_uint32 major_status = GSS_S_UNAVAILABLE;
    OM_uint32 tmpMinor;
    int more = -1;

    *user_ok = 0;

    if (!gss_oid_equal(&user->gn_type, GSS_C_NT_USER_NAME))
        return GSS_S_BAD_NAMETYPE;

    while (more != 0 && *user_ok == 0) {
	gss_buffer_desc value;
	gss_buffer_desc display_value;
	int authenticated = 0, complete = 0;

	major_status = gss_get_name_attribute(minor_status,
					      (gss_name_t)name,
					      GSS_C_ATTR_LOCAL_LOGIN_USER,
					      &authenticated,
					      &complete,
					      &value,
					      &display_value,
					      &more);
	if (GSS_ERROR(major_status))
	    break;

	if (authenticated &&
	    value.length == user->gn_value.length &&
	    memcmp(value.value, user->gn_value.value, user->gn_value.length) == 0)
	    *user_ok = 1;

	gss_release_buffer(&tmpMinor, &value);
	gss_release_buffer(&tmpMinor, &display_value);
    }

    return major_status;
}

OM_uint32
gss_authorize_localname(OM_uint32 *minor_status,
	                const gss_name_t gss_name,
	                const gss_name_t gss_user,
	                int *user_ok)

{
    OM_uint32 major_status;
    const struct _gss_name *name = (const struct _gss_name *) gss_name;
    const struct _gss_name *user = (const struct _gss_name *) gss_user;

    *minor_status = 0;
    *user_ok = 0;

    if (gss_name == GSS_C_NO_NAME || gss_user == GSS_C_NO_NAME)
        return GSS_S_CALL_INACCESSIBLE_READ;

    /* name must not be a MN */
    if (HEIM_SLIST_FIRST(&user->gn_mn) != NULL)
        return GSS_S_BAD_NAME;

    /* If mech returns yes, we return yes */
    major_status = mech_authorize_localname(minor_status,
                                            name, user, user_ok);
    if (major_status == GSS_S_COMPLETE && *user_ok)
	return GSS_S_COMPLETE;

    /* If attribute exists, we evaluate attribute */
    if (attr_authorize_localname(minor_status,
                                 name, user, user_ok) == GSS_S_COMPLETE)
	return GSS_S_COMPLETE;

    /* If mech returns unavail, we compare the local name */
    if (major_status == GSS_S_UNAVAILABLE)
        major_status = gss_compare_name(minor_status, gss_name,
                                        gss_user, user_ok);

    return major_status;
}

int
gss_userok(const gss_name_t name,
           const char *user)
{
    OM_uint32 major_status, minor_status;
    gss_buffer_desc userBuf;
    gss_name_t userName;
    int user_ok = 0;

    userBuf.value = (void *)user;
    userBuf.length = strlen(user);

    major_status = gss_import_name(&minor_status, &userBuf,
                                   GSS_C_NT_USER_NAME, &userName);
    if (GSS_ERROR(major_status))
        return 0;

    major_status = gss_authorize_localname(&minor_status, name, userName, &user_ok);
    if (GSS_ERROR(major_status))
        user_ok = 0;

    gss_release_name(&minor_status, &userName);

    return user_ok;
}
