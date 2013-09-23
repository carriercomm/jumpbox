#include "djb.h"

#define DJB_WORKERS	8
#define DJB_HOST	"localhost"
#define DJB_PORT	6543

typedef struct {
	hnode_t			node;		/* List node */
	httpsrv_client_t	*hcl;		/* Client for this request */
} djb_req_t;

/* New, unforwarded queries (awaiting 'pull') */
hlist_t lst_proxy_new;

/* Outstanding queries (answer to a 'pull', awaiting 'push') */
hlist_t lst_proxy_out;

/* Requests that want a 'pull', waiting for a 'proxy_new' entry */
hlist_t lst_api_pull;

typedef struct {
	char	httpcode[64];
	char	httptext[(256-64-32)];
	char	seqno[32];
	char	setcookie[8192];
	char	cookie[8192];
} djb_headers_t;

#define DJBH(h) offsetof(djb_headers_t, h), sizeof (((djb_headers_t *)NULL)->h)

misc_map_t djb_headers[] = {
	{ "DJB-HTTPCode",	DJBH(httpcode)	},
	{ "DJB-HTTPText",	DJBH(httptext)	},
	{ "DJB-SeqNo",		DJBH(seqno)	},

	/* Server -> Client */
	{ "DJB-Set-Cookie",	DJBH(setcookie)	},

	/* Client -> Server */
	{ "Cookie",		DJBH(cookie)	},
	{ NULL,			0, 0		}
};

void
djb_html_css(httpsrv_client_t *hcl);
void
djb_html_css(httpsrv_client_t *hcl) {
	/* Can Cache This */
	httpsrv_expire(hcl, HTTPSRV_EXPIRE_LONG);

	/* HTML header */
	conn_put(&hcl->conn,
		"/* JumpBox CSS */\n"
		"form label\n"
		"{\n"
		"	float		: left;\n"
		"	width		: 120px;\n"
		"	font-weight	: bold;\n"
		"}\n"
		"\n"
		"input, textarea\n"
		"{\n"
		"	margin-bottom	: 5px;\n"
		"}\n"
		"\n"
		"textarea\n"
		"{\n"
		"	width		: 250px;\n"
		"	height		: 150px;\n"
		"}\n"
		"\n"
		"form input[type=submit]\n"
		"{\n"
		"	margin-left	: 120px;\n"
		"	margin-top	: 5px;\n"
		"	width		: 90px;\n"
		"}\n"
		"form br\n"
		"{\n"
		"	clear		: left;\n"
		"}\n"
		"\n"
		"th\n"
		"{\n"
		"	background-color: #eeeeee;\n"
		"}\n"
		"\n"
		".header, .footer\n"
		"{\n"
		"	background-color: #eeeeee;\n"
		"	width		: 100%;\n"
		"}\n"
		"\n"
		".footer\n"
		"{\n"
		"	margin-top	: 5em;\n"
		"}\n"
		"\n"
		"a\n"
		"{\n"
		"	background-color: transparent;\n"
		"	text-decoration	: none;\n"
		"	border-bottom	: 1px solid #bbbbbb;\n"
		"	color		: black;\n"
		"}\n"
		"\n"
		"a:hover\n"
		"{\n"
		"	color		: #000000;\n"
		"	text-decoration	: none;\n"
		"	background-color: #d1dfd5;\n"
		"	border-bottom	: 1px solid #a8bdae;\n"
		"}\n"
		"\n"
		".right\n"
		"{\n"
		"	text-align	: right\n"
		"}\n"
		"\n"
		"div.status\n"
		"{\n"
		"	overflow	: hidden;\n"
		"	background-color: #eeeeee;\n"
		"}\n"
		"\n"
		"div.status div.statuslabel\n"
		"{\n"
		"	float		: left;\n"
		"	font-weight	: bold;\n"
		"	padding-right	: 0.5em;\n"
		"}\n"
		"\n"
		"div.status div.statustext\n"
		"{\n"
		"	float		: left;\n"
		"}\n"
		"\n"
		);
}

void
djb_html_top(httpsrv_client_t *hcl, void UNUSED *user);
void
djb_html_top(httpsrv_client_t *hcl, void UNUSED *user) {
	conn_put(&hcl->conn,
		"<!doctype html>\n"
		"<html lang=\"en\">\n"
		"<head>\n"
		"<title>DEFIANCE JumpBox</title>\n"
		"<link rel=\"icon\" type=\"image/png\" "
		"href=\"http://www.farsightsecurity.com/favicon.ico\">\n"
		"<link rel=\"stylesheet\" type=\"text/css\" href=\"/djb.css\" />\n"
		"</head>\n"
		"<body>\n"
		"<div class=\"header\">\n"
		"SAFER DEFIANCE :: JumpBox\n"
		"</div>\n");
}

void
djb_html_tail(httpsrv_client_t *hcl, void UNUSED *user);
void
djb_html_tail(httpsrv_client_t *hcl, void UNUSED *user) {
	conn_put(&hcl->conn,
		"<div class=\"footer\">\n"
		"SAFER DEFIANCE :: Defiance JumpBox (djb) by "
		"<a href=\"http://www.farsightsecurity.com\">"
		"Farsight Security, Inc"
		"</a>.\n"
		"</div>\n"
		"</body>\n"
		"</html>\n");
}

void
djb_error(httpsrv_client_t *hcl, unsigned int code, const char *msg) {
	httpsrv_error(hcl, code, msg);
	httpsrv_expire(hcl, HTTPSRV_EXPIRE_FORCE);
	httpsrv_done(hcl);
}

void
djb_result(httpsrv_client_t *hcl, const char *msg) {
	httpsrv_answer(hcl, HTTPSRV_HTTP_OK, HTTPSRV_CTYPE_JSON);
	httpsrv_expire(hcl, HTTPSRV_EXPIRE_FORCE);
	conn_put(&hcl->conn, msg);
	httpsrv_done(hcl);
}

/* API-Pull, requests a new Proxy-request */
djb_req_t *
djb_find_hcl(hlist_t *lst, httpsrv_client_t *hcl);
djb_req_t *
djb_find_hcl(hlist_t *lst, httpsrv_client_t *hcl) {
	djb_req_t	*r, *rn, *pr = NULL;

	fassert(lst != NULL);
	fassert(hcl != NULL);

	/* Find this HCL in our outstanding proxy requests */
	list_lock(lst);
	list_for(lst, r, rn, djb_req_t *) {
		if (r->hcl != hcl) {
			continue;
		}

		/* Gotcha */
		pr = r;

		/* Remove it from this list */
		list_remove(lst, &pr->node);
		break;
	}
	list_unlock(lst);

	if (pr != NULL) {
		logline(log_DEBUG_, HCL_ID, pr->hcl->id);
	} else {
		logline(log_WARNING_,
			"No such HCL (" HCL_ID ") found!?",
			hcl->id);
	}

	return (pr);
}

djb_req_t *
djb_find_req(hlist_t *lst, uint64_t id, uint64_t reqid);
djb_req_t *
djb_find_req(hlist_t *lst, uint64_t id, uint64_t reqid) {
	djb_req_t	*r, *rn, *pr = NULL;

	/* Find this Request-ID in our outstanding proxy requests */
	list_lock(lst);
	list_for(lst, r, rn, djb_req_t *) {
		if (r->hcl->id != id ||
		    r->hcl->reqid != reqid) {
			continue;
		}

		/* Gotcha */
		pr = r;

		/* Remove it from this list */
		list_remove(lst, &pr->node);
		break;
	}
	list_unlock(lst);

	if (pr != NULL) {
		logline(log_DEBUG_,
			"%" PRIu64 ":%" PRIu64 " = " HCL_ID,
			id, reqid, pr->hcl->id);
	} else {
		logline(log_WARNING_,
			"No such HCL (%" PRIu64 ":%" PRIu64 " found!?",
			id, reqid);
	}

	return (pr);
}

djb_req_t *
djb_find_req_dh(httpsrv_client_t *hcl, hlist_t *lst, djb_headers_t *dh);
djb_req_t *
djb_find_req_dh(httpsrv_client_t *hcl, hlist_t *lst, djb_headers_t *dh) {
	djb_req_t	*pr;
	uint64_t	id, reqid;

	/* We require a DJB-HTTPCode */
	if (strlen(dh->httpcode) == 0) {
		djb_error(hcl, 504, "Missing DJB-HTTPCode");
		return NULL;
	}

	/* We require a DJB-HTTPText */
	if (strlen(dh->httptext) == 0) {
		djb_error(hcl, 504, "Missing DJB-HTTPText");
		return NULL;
	}

	/* Convert the Request-ID */
	if (sscanf(dh->seqno, "%09" PRIx64 "%09" PRIx64, &id, &reqid) != 2) {
		djb_error(hcl, 504, "Missing or malformed DJB-SeqNo");
		return NULL;
	}

	pr = djb_find_req(lst, id, reqid);
	if (!pr) {
		djb_error(hcl, 404, "No such request outstanding");
	}

	return (pr);
}

void
djb_pull_post(httpsrv_client_t *hcl);
void
djb_pull_post(httpsrv_client_t *hcl) {
	djb_req_t		*ar;
	uint64_t		id = hcl->id;

	logline(log_DEBUG_, HCL_ID, id);

	/* Proxy request - add it to the requester list */
	ar = mcalloc(sizeof *ar, "djb_req_t *");
	if (!ar) {
		djb_error(hcl, 500, "Out of memory");

		/* XXX: All is lost here, maybe cleanup hcl? */
		return;
	}

	/* Fill in the details */
	ar->hcl = hcl;

	list_addtail_l(&lst_api_pull, &ar->node);

	logline(log_DEBUG_, HCL_ID " done", id);
}

bool
djb_pull(httpsrv_client_t *hcl);
bool
djb_pull(httpsrv_client_t *hcl){
	logline(log_DEBUG_, HCL_ID " keephandling=yes", hcl->id);

	/*
	 * Add this request to the queue when the request is handled
	 * The manager will divide the work
	 */
	hcl->keephandling = true;
	httpsrv_set_posthandle(hcl, djb_pull_post);

	/* No need to read from it further for the moment */
	return (true);
}

/* Push request, answer to a pull request */
bool
djb_push(httpsrv_client_t *hcl, djb_headers_t *dh);
bool
djb_push(httpsrv_client_t *hcl, djb_headers_t *dh) {
	djb_req_t *pr;

	logline(log_DEBUG_, HCL_ID, hcl->id);

	/* Find the request */
	pr = djb_find_req_dh(hcl, &lst_proxy_out, dh);
	if (pr == NULL) {
		/* Can happen if the request timed out etc */
		logline(log_DEBUG_,
			HCL_ID " request timed out",
			hcl->id);
		httpsrv_done(hcl);
		return (true);
	}

	logline(log_DEBUG_,
		HCL_ID " -> " HCL_ID,
		hcl->id, pr->hcl->id);

	/* Connections might close before the answer is returned */
	if (!conn_is_valid(&pr->hcl->conn)) {
		logline(log_DEBUG_,
			HCL_ID " " CONN_ID " closed",
			hcl->id, conn_id(&pr->hcl->conn));
		httpsrv_done(hcl);
		return (true);
	}

	/* We got an answer, send back what we have already */
	httpsrv_answer(pr->hcl, atoi(dh->httpcode), dh->httptext, NULL);

	/* Server to Client */
	if (strlen(dh->setcookie) > 0) {
		conn_addheaderf(&pr->hcl->conn, "Set-Cookie: %s",
				dh->setcookie);
	}

	/* Add all the headers we received */
	/* XXX: We should scrub DJB-SeqNo */
	buf_lock(&hcl->the_headers);
	conn_addheaders(&pr->hcl->conn, buf_buffer(&hcl->the_headers));
	buf_unlock(&hcl->the_headers);

	if (hcl->headers.content_length == 0) {
		/* Send back a 200 OK as we proxied it */
		logline(log_DEBUG_, "API push done (no content)");

		/* This request is done (after flushing) */
		httpsrv_done(pr->hcl);

		/* Release it */
		free(pr);

		/* HTTP okay */
		httpsrv_answer(hcl, HTTPSRV_HTTP_OK, HTTPSRV_CTYPE_HTML);

		/* A message as a body (Content-Length is arranged by conn) */
		conn_printf(&hcl->conn, "Push Forward successful\r\n");

		/* Done with this */
		httpsrv_done(hcl);

		return (false);
	}

	/* Still need to forward the body as there is length */

	/* The Content-Length header is already included in all the headers */
	conn_add_contentlen(&pr->hcl->conn, false);

	logline(log_DEBUG_,
		"Forwarding body from " HCL_ID " to " HCL_ID,
		pr->hcl->id, hcl->id);

	/* Forward the body from hcl to pr */
	httpsrv_forward(hcl, pr->hcl);

	/* Free it up, not tracked anymore */
	free(pr);

	/* No need to read from it further for the moment */
	return (true);
}

/* hcl == the client proxy request, pr->hcl = pull API request */
void
djb_bodyfwd_done(httpsrv_client_t *hcl, httpsrv_client_t *fhcl, void UNUSED *user);
void
djb_bodyfwd_done(httpsrv_client_t *hcl, httpsrv_client_t *fhcl, void UNUSED *user) {

	logline(log_DEBUG_,
		"Done forwarding body from "
		HCL_ID " (keephandling=%s) to "
		HCL_ID " (keephandling=%s)",
		hcl->id, yesno(hcl->keephandling),
		fhcl->id, yesno(fhcl->keephandling));

	/* Send a content-length again if there is one */
	conn_add_contentlen(&fhcl->conn, true);

	/* Was this a push? Then we answer that it is okay */
	if (strncasecmp(hcl->headers.uri, "/push/", 6) == 0) {
		/* Send back a 200 OK as we proxied it */
		logline(log_DEBUG_, "API push, done with it");

		/* HTTP okay */
		httpsrv_answer(hcl, HTTPSRV_HTTP_OK, HTTPSRV_CTYPE_HTML);

		/* A message as a body (Content-Length is arranged by conn) */
		conn_printf(&hcl->conn, "Push Body Forward successful\r\n");
	} else {
		/* This was a proxy-POST, thus add it back to process queue */
		logline(log_DEBUG_, "proxy-POST, adding back to queue");
	}

	/* Served another one */
	thread_serve();

	/* Done for this request is handled by caller: httpsrv_handle_http() */
	logline(log_DEBUG_, "end");
}

void
djb_accept(httpsrv_client_t *hcl, void UNUSED *user);
void
djb_accept(httpsrv_client_t *hcl, void UNUSED *user) {

	djb_headers_t *dh;

	logline(log_DEBUG_, HCL_ID, hcl->id);

	dh = mcalloc(sizeof *dh, "djb_headers_t *");
	if (!dh) {
		djb_error(hcl, 500, "Out of memory");
		return;
	}

	logline(log_DEBUG_,
		HCL_ID " %p",
		hcl->id, (void *)dh);

	httpsrv_set_userdata(hcl, dh);
}

void
djb_header(httpsrv_client_t UNUSED *hcl, void *user, char *line);
void
djb_header(httpsrv_client_t UNUSED *hcl, void *user, char *line) {
	djb_headers_t *dh = (djb_headers_t *)user;

	misc_map(line, djb_headers, (char *)dh);
}

static void
djb_status_list(httpsrv_client_t *hcl, hlist_t *lst,
		const char *title, const char *desc);
static void
djb_status_list(httpsrv_client_t *hcl, hlist_t *lst,
		const char *title, const char *desc) {
	djb_req_t	*r, *rn;
	unsigned int	cnt = 0;

	conn_printf(&hcl->conn,
		"<h1>List: %s</h1>\n"
		"<p>\n"
		"%s.\n"
		"</p>\n",
		title,
		desc);

	list_lock(lst);
	list_for(lst, r, rn, djb_req_t *) {

		if (cnt == 0) {
			conn_put(&hcl->conn,
				"<table>\n"
				"<tr>\n"
				"<th>ID</th>\n"
				"<th>ReqID</th>\n"
				"<th>Host</th>\n"
				"<th>Request</th>\n"
				"</tr>\n");
		}

		conn_printf(&hcl->conn,
			"<tr>"
			"<td>" HCL_IDn "</td>"
			"<td>%" PRIu64 "</td>"
			"<td>%s</td>"
			"<td>%s</td>"
			"</tr>\n",
			r->hcl->id,
			r->hcl->reqid,
			r->hcl->headers.hostname,
			r->hcl->the_request);
		cnt++;
	}
	list_unlock(lst);

	if (cnt == 0) {
		conn_put(&hcl->conn,
			"No outstanding requests.");
	} else {
		conn_put(&hcl->conn,
			"</table>\n");
	}
}

static void
djb_status_threads_cb(	void		*cbdata,
			uint64_t	tnum,
			uint64_t	tid,
			const char	*starttime,
			uint64_t	runningsecs,
			const char	*description,
			bool		thisthread,
			const char	*state,
			const char	*message,
			uint64_t	served);
static void
djb_status_threads_cb(  void		*cbdata,
			uint64_t	tnum,
			uint64_t	tid,
			const char	*starttime,
			uint64_t	runningsecs,
			const char	*description,
			bool		thisthread,
			const char	*state,
			const char	*message,
			uint64_t	served)
{
	httpsrv_client_t *hcl = (httpsrv_client_t *)cbdata;

	conn_printf(&hcl->conn,
		"<tr>"
		"<td>%" PRIu64 "</td>"
		"<td>" THREAD_IDn "</td>"
		"<td>%s</td>"
		"<td>%" PRIu64 "</td>"
		"<td>%s</td>"
		"<td>%s</td>"
		"<td>%s</td>"
		"<td>%s</td>"
		"<td>%" PRIu64 "</td>"
		"</tr>\n",
		tnum,
		tid,
		starttime,
		runningsecs,
		description,
		yesno(thisthread),
		state,
		message,
		served);
}

static void
djb_status_threads(httpsrv_client_t *hcl);
static void
djb_status_threads(httpsrv_client_t *hcl) {
	unsigned int cnt;

	conn_put(&hcl->conn,
		"<h1>Threads</h1>\n"
		"<p>\n"
		"Threads running inside JumpBox.\n"
		"</p>\n"
		"<table>\n"
		"<tr>\n"
		"<th>No</th>\n"
		"<th>TID</th>\n"
		"<th>Start Time</th>\n"
		"<th>Running seconds</th>\n"
		"<th>Description</th>\n"
		"<th>This Thread</th>\n"
		"<th>State</th>\n"
		"<th>Message</th>\n"
		"<th>Served</th>\n"
		"</tr>\n");

	cnt = thread_list(djb_status_threads_cb, hcl);

	conn_put(&hcl->conn,
			"</table>\n");

	if (cnt == 0) {
		conn_put(&hcl->conn,
				"Odd, no threads where found running!?");
	}
}

static void
djb_status_httpsrv(httpsrv_client_t *hcl);
static void
djb_status_httpsrv(httpsrv_client_t *hcl) {
	conn_put(&hcl->conn,
		"<h2>JumpBox HTTP Server Sessions</h2>\n"
		"<p>\n"
		"Following sessions are currently running in the HTTP Server.\n"
		"</p>\n"
		"\n");

	httpsrv_sessions(hcl);
}

static void
djb_status_version(httpsrv_client_t *hcl);
static void
djb_status_version(httpsrv_client_t *hcl) {
	conn_put(&hcl->conn,
		"<h2>JumpBox Information</h2>\n"
		"<p>\n"
		"Following details are available about this JumpBox (djb).\n"
		"</p>\n"
		"\n"
		"<table>\n"
		"<tr><th>Version:</th><td>" STR(PROJECT_VERSION) "</td></tr>"
		"<tr><th>Buildtime:</th><td>" STR(PROJECT_BUILDTIME) "</td></tr>"
		"<tr><th>GIT hash:</th><td>" STR(PROJECT_GIT) "</td></tr>"
		"</table>\n");
}

void
djb_status(httpsrv_client_t *hcl);
void
djb_status(httpsrv_client_t *hcl) {
	httpsrv_answer(hcl, HTTPSRV_HTTP_OK, HTTPSRV_CTYPE_HTML);

	/* Body is just JumpBox (Content-Length is arranged by conn) */
	djb_html_top(hcl, NULL);

	djb_status_version(hcl);
	djb_status_threads(hcl);

	djb_status_list(hcl, &lst_proxy_new,
			"Proxy New",
			"New unforwarded requests");

	djb_status_list(hcl, &lst_proxy_out,
			"Proxy Out",
			"Outstanding queries "
			"(answer to pull, waiting for a push)");

	djb_status_list(hcl, &lst_api_pull,
			"API Pull",
			"Requests that want a pull, "
			"waiting for proxy_new entry");

	djb_status_httpsrv(hcl);

	djb_html_tail(hcl, NULL);

	/* This request is done */
	httpsrv_done(hcl);
}

bool
djb_handle_api(httpsrv_client_t *hcl, djb_headers_t *dh);
bool
djb_handle_api(httpsrv_client_t *hcl, djb_headers_t *dh) {

	/* A DJB API request */
	logline(log_DEBUG_,
		HCL_ID " DJB API request: %s",
		hcl->id, hcl->headers.uri);

	/* Our API URIs                    123456  */
	if (strncasecmp(hcl->headers.uri, "/pull/", 6) == 0) {
		return djb_pull(hcl);

	} else if (strncasecmp(hcl->headers.uri, "/push/", 6) == 0) {
		return djb_push(hcl, dh);

	} else if (strcasecmp(hcl->headers.uri, "/shutdown/") == 0) {
		djb_error(hcl, 200, "Shutting down");
		thread_stop_running();
		return (true);

	/*					  12345 */
	} else if (strncasecmp(hcl->headers.uri, "/acs/", 5) == 0) {
		return (acs_handle(hcl));

	/*					  123456789012 */
	} else if (strncasecmp(hcl->headers.uri, "/rendezvous/", 12) == 0) {
#ifdef DJB_RENDEZVOUS
		rdv_handle(hcl);
#else
		djb_error(hcl, 500, "Rendezvous module not enabled");
#endif
		return (false);
	/*					   123456789012 */
	} else if (strncasecmp(hcl->headers.uri, "/preferences/", 12) == 0) {
		prf_handle(hcl);
		return (false);

	} else if (strcasecmp(hcl->headers.uri, "/") == 0) {
		djb_status(hcl);
		return (false);

	} else if (strcasecmp(hcl->headers.uri, "/djb.css") == 0) {
		httpsrv_answer(hcl, HTTPSRV_HTTP_OK, HTTPSRV_CTYPE_CSS);

		djb_html_css(hcl);

		httpsrv_done(hcl);
		return (false);
	}

	/* Not a valid API request */
	djb_error(hcl, 404, "No such DJB API request");
	return (false);
}

void
djb_handle_proxy_post(httpsrv_client_t *hcl);
void
djb_handle_proxy_post(httpsrv_client_t *hcl) {
	djb_req_t	*pr;
	uint64_t	id = hcl->id;

	logline(log_DEBUG_, HCL_ID, id);

	/* Proxy request - add it to the requester list */
	pr = mcalloc(sizeof *pr, "djb_req_t *");
	if (!pr) {
		djb_error(hcl, 500, "Out of memory");

		/* XXX: All is lost here, maybe cleanup hcl? */
		return;
	}

	/* Fill in the details */
	pr->hcl = hcl;

	/*
	 * Add this request to the queue
	 * The manager will divide the work
	 */
	list_addtail_l(&lst_proxy_new, &pr->node);

	logline(log_DEBUG_, HCL_ID " done", id);

	return;
}

bool
djb_proxy_add(httpsrv_client_t *hcl) {
	logline(log_DEBUG_, HCL_ID " keephandling=yes", hcl->id);

	/*
	 * Add this request to the queue when the request is handled
	 * The manager will divide the work
	 */
	hcl->keephandling = true;
	httpsrv_set_posthandle(hcl, djb_handle_proxy_post);

	return (true);
}

bool
djb_handle_proxy(httpsrv_client_t *hcl);
bool
djb_handle_proxy(httpsrv_client_t *hcl) {
	static const char	*hostname = NULL;
	static bool		got_hostname = false;

	/* Only fetch this once, ever */
	if (got_hostname == false) {
		got_hostname = true;

		/*
		 * Force the hostname to something else
		 * than what the requestor wants?
		 */
		hostname = getenv("DJB_FORCED_HOSTNAME");

		if (hostname) {
			logline(log_DEBUG_,
				"Proxy Hostname Override: %s",
				hostname);
		}
	}

	/* Override the hostname? */
	if (hostname != NULL) {
		strncpy(hcl->headers.hostname,
			hostname,
			sizeof hcl->headers.hostname);
		logline(log_DEBUG_,
			"Forcing Hostname to: %s",
			hcl->headers.hostname);
	}

	/* Add it to the proxy queue */
	return (djb_proxy_add(hcl));
}

bool
djb_handle(httpsrv_client_t *hcl, void *user);
bool
djb_handle(httpsrv_client_t *hcl, void *user) {
	djb_headers_t	*dh = (djb_headers_t *)user;
	bool		done;

	logline(log_DEBUG_,
		HCL_ID " hostname: %s uri: %s",
		hcl->id, hcl->headers.hostname, hcl->headers.uri);

	/* Parse the request */
	if (!httpsrv_parse_request(hcl)) {
		/* function has added error already */
		return (true);
	}

	/* Is this a DJB API or a proxy request? */
	if (	strcmp(hcl->headers.hostname, "127.0.0.1:6543"	) == 0 ||
		strcmp(hcl->headers.hostname, "[::1]:6543"	) == 0 ||
		strcmp(hcl->headers.hostname, "localhost:6543"	) == 0) {

		/* API request */
		done = djb_handle_api(hcl, dh);
	} else {
		/* Proxied request */
		done = djb_handle_proxy(hcl);
	}

	logline(log_DEBUG_, HCL_ID " end", hcl->id);

	return (done);
}

void
djb_done(httpsrv_client_t *hcl, void *user);
void
djb_done(httpsrv_client_t *hcl, void *user) {
	djb_headers_t  *dh;

	logline(log_DEBUG_,
		HCL_ID " %p",
		hcl->id, user);

	dh = (djb_headers_t *)user;
	assert(dh != NULL);
	memzero(dh, sizeof *dh);
}

void
djb_close(httpsrv_client_t *hcl, void UNUSED *user);
void
djb_close(httpsrv_client_t *hcl, void UNUSED *user) {

	logline(log_DEBUG_, HCL_ID, hcl->id);
}

/*
 * pr = client request
 * ar = /pull/ API request
 */
static void
djb_handle_forward(djb_req_t *pr, djb_req_t *ar);
static void
djb_handle_forward(djb_req_t *pr, djb_req_t *ar) {
	djb_headers_t	*dh;

	fassert(pr->hcl);
	fassert(ar->hcl);

	logline(log_DEBUG_,
		"request " HCL_ID ", puller " HCL_ID,
		pr->hcl->id, ar->hcl->id);

	/* This has to be valid */
	fassert(conn_is_valid(&pr->hcl->conn));

	/* Connection might have closed by now */
	if (!conn_is_valid(&ar->hcl->conn)) {
		fassert(false);
		/* This request is done */
		httpsrv_done(ar->hcl);
		return;
	}

	/* pr's headers */
	dh = (djb_headers_t *)pr->hcl->user;

	/* HTTP okay */
	httpsrv_answer(ar->hcl, HTTPSRV_HTTP_OK, NULL);

	/* DJB headers */
	conn_addheaderf(&ar->hcl->conn, "DJB-URI: http://%s%s",
			pr->hcl->headers.hostname,
			pr->hcl->headers.rawuri);

	conn_addheaderf(&ar->hcl->conn, "DJB-Method: %s",
			httpsrv_methodname(pr->hcl->method));

	conn_addheaderf(&ar->hcl->conn, "DJB-SeqNo: %09" PRIx64 "%09" PRIx64,
			pr->hcl->id, pr->hcl->reqid);

	assert(pr->hcl->method != HTTP_M_NONE);

	/* Client to server */
	if (strlen(dh->cookie) > 0) {
		conn_addheaderf(&ar->hcl->conn, "DJB-Cookie: %s",
				dh->cookie);
	}

	if (pr->hcl->method != HTTP_M_POST) {
		logline(log_DEBUG_,
			"req " HCL_ID " with puller " HCL_ID " is non-POST",
			pr->hcl->id, ar->hcl->id);

		/* XHR requires a return, thus just give it a blank body */
		conn_addheaderf(&ar->hcl->conn, "Content-Type: text/html");

		/* Empty-ish body (Content-Length is arranged by conn) */
		conn_printf(&ar->hcl->conn, "Non-POST JumpBox response\r\n");

		/* Put this on the proxy_out list */
		list_addtail_l(&lst_proxy_out, &pr->node);

		/* This request is done */
		httpsrv_done(ar->hcl);

		/* Done handling this connection */
		connset_handling_done(&ar->hcl->conn, false);

	} else {
		/* POST request */
		logline(log_DEBUG_,
			"req " HCL_ID " with puller " HCL_ID " is POST",
			pr->hcl->id, ar->hcl->id);

		/* Add the content type of the data to come */
		conn_addheaderf(&ar->hcl->conn, "Content-Type: %s",
				strlen(pr->hcl->headers.content_type) > 0 ?
					pr->hcl->headers.content_type :
					"text/html");

		/* Is there no body, then nothing further to do */
		if (pr->hcl->headers.content_length == 0) {
			/* Put this on the proxy_out list */
			list_addtail_l(&lst_proxy_out, &pr->node);

			/* This request is done (after flushing) */
			httpsrv_done(ar->hcl);

			/* Done handling this connection */
			connset_handling_done(&ar->hcl->conn, false);
		} else {
			/* Put this on the proxy_out list */
			list_addtail_l(&lst_proxy_out, &pr->node);

			logline(log_DEBUG_,
				"Forwarding POST body from "
				HCL_ID " (keephandling=%s) to " HCL_ID " (keephandling=%s)",
				pr->hcl->id, yesno(pr->hcl->keephandling),
				ar->hcl->id, yesno(ar->hcl->keephandling));

			/* Forward the body from pr->hcl to ar->hcl */
			httpsrv_forward(pr->hcl, ar->hcl);
		}
	}

	logline(log_DEBUG_, "end");
}

static void *
djb_worker_thread(void UNUSED *arg);
static void *
djb_worker_thread(void UNUSED *arg) {
	djb_req_t	*pr, *ar;

	logline(log_DEBUG_, "...");

	while (thread_keep_running()) {
		logline(log_DEBUG_, "waiting for proxy request");

		thread_setmessage("Waiting for Proxy Request");

		/* Get a new proxy request */
		pr = (djb_req_t *)list_getnext(&lst_proxy_new);
		if (pr == NULL) {
			if (thread_keep_running()) {
				logline(log_ERR_,
					" get_next(proxy_new) failed...");
			}
			break;
		}

		fassert(pr->hcl);

		logline(log_DEBUG_, "got request " HCL_ID ", getting puller",
			pr->hcl->id);
		thread_setmessage("Got Request " HCL_ID, pr->hcl->id);

		/* We got a request, get a puller for it */
		ar = (djb_req_t *)list_getnext(&lst_api_pull);

		if (ar == NULL) {
			if (thread_keep_running()) {
				logline(log_ERR_,
					" get_next(api_pull) failed...");
			}
			break;
		}

		logline(log_DEBUG_, "request " HCL_ID ", puller "HCL_ID,
			pr->hcl->id, ar->hcl->id);

		thread_setmessage("request " HCL_ID " puller " HCL_ID,
				  pr->hcl->id, ar->hcl->id);

		djb_handle_forward(pr, ar);

		/* Release it */
		free(ar);

		/* Served another one */
		thread_serve();

		logline(log_DEBUG_, "end");
	}

	logline(log_DEBUG_, "exit");

	return (NULL);
}

static int
djb_run(void);
static int
djb_run(void) {
	httpsrv_t	*hs = NULL;
	int		ret = 0;
	unsigned int	i;

	/* Create out DGW structure */
	hs = (httpsrv_t *)mcalloc(sizeof *hs, "httpsrv_t");
	if (hs == NULL) {
		logline(log_CRIT_, "No memory for HTTP Server");
		return (-1);
	}

	while (true) {
		/* Launch a few worker threads */
		for (i = 0; i < DJB_WORKERS; i++) {
			if (!thread_add("DJBWorker", &djb_worker_thread, NULL)) {
				logline(log_ERR_, "could not create thread");
				ret = -1;
				break;
			}
		}

		/* Initialize a HTTP Server */
		if (!httpsrv_init(hs, NULL,
				  djb_html_top,
				  djb_html_tail,
				  djb_accept,
				  djb_header,
				  djb_handle,
				  djb_bodyfwd_done,
				  djb_done,
				  djb_close)) {
			logline(log_ERR_, "Could not initialize HTTP server");
			ret = -1;
			break;
		}

		/* Fire up an HTTP server */
		if (!httpsrv_start(hs, DJB_HOST, DJB_PORT, DJB_WORKERS)) {
			logline(log_ERR_, "HTTP Server failed");
			ret = -1;
			break;
		}

		/* Nothing more to set up */
		break;
	}

	/* Just sleep over here */
	while (ret == 0 && thread_keep_running()) {
		thread_sleep(5000);
	}

	/* Cleanup time as the mainloop ended */
	logline(log_DEBUG_, "Cleanup Time...(main ret = %d)", ret);

	/* Make sure that our threads are done */
	thread_stopall(false);

	/* Cleanup */
	acs_set_net(NULL);

	/* Clean up the http object */
	if (hs)
		httpsrv_exit(hs);

	return (ret);
}

static void
djb_usage(const char *progname);
static void
djb_usage(const char *progname) {
	fprintf(stderr, "Usage: %s [<command>]\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "run        = run the server\n");
	fprintf(stderr, "daemonize [<pidfilename>|''] "
				  "[<logfilename>|''] "
				  "[<username>]\n");
	fprintf(stderr, "           = daemonize the server into\n");
	fprintf(stderr, "             the background\n");
}

int
main(int argc, const char *argv[]) {
	int ret = 0;

	/* Setup logging */
	log_setup("djb", stderr);

	if (!thread_init())
		return (-1);

	list_init(&lst_proxy_new);
	list_init(&lst_proxy_out);
	list_init(&lst_api_pull);

	if (argc < 2) {
		djb_usage(argv[0]);
		ret = -1;
	} else if (strcasecmp(argv[1], "daemonize") == 0 &&
			(argc >= 2 || argc <= 5)) {

		/* Setup the log (before setuid) */
		if (argc >= 4 && strlen(argv[3]) > 0) {
			if (!log_set(argv[3])) {
				ret = -1;
			}
		}

		if (ret != -1) {
			ret = thread_daemonize(
					argc >= 1 ? argv[2] : NULL,
					argc >= 3 ? argv[4] : NULL);
			if (ret > 0) {
				ret = djb_run();
			}
		}
	} else if (strcasecmp(argv[1], "run") == 0 && argc == 2) {
		ret = djb_run();
	} else {
		djb_usage(argv[0]);
		ret = -1;
	}

	logline(log_DEBUG_, "It's all over... (%d)", ret);

	thread_exit();

	/* All okay */
	return (ret);
}

