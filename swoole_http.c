/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole.h"
#include "swoole_http.h"

#include <ext/standard/url.h>
#include <ext/standard/sha1.h>
#include <ext/standard/php_var.h>
#include <ext/standard/php_string.h>
#include <ext/standard/php_math.h>
#include <ext/date/php_date.h>

#include <main/php_variables.h>

#include "websocket.h"
#include "Connection.h"
#include "base64.h"

static swArray *http_client_array;
static uint8_t http_merge_global_flag = 0;
static uint8_t http_merge_request_flag = 0;

enum http_response_flag
{
    HTTP_RESPONSE_SERVER           = 1u << 1,
    HTTP_RESPONSE_CONNECTION       = 1u << 2,
    HTTP_RESPONSE_CONTENT_LENGTH   = 1u << 3,
    HTTP_RESPONSE_DATE             = 1u << 4,
    HTTP_RESPONSE_CONTENT_TYPE     = 1u << 5,
};

enum http_global_flag
{
    HTTP_GLOBAL_GET       = 1u << 1,
    HTTP_GLOBAL_POST      = 1u << 2,
    HTTP_GLOBAL_COOKIE    = 1u << 3,
    HTTP_GLOBAL_REQUEST   = 1u << 4,
    HTTP_GLOBAL_SERVER    = 1u << 5,
};

zend_class_entry swoole_http_server_ce;
zend_class_entry *swoole_http_server_class_entry_ptr;

zend_class_entry swoole_http_response_ce;
zend_class_entry *swoole_http_response_class_entry_ptr;

zend_class_entry swoole_http_request_ce;
zend_class_entry *swoole_http_request_class_entry_ptr;

static zval* php_sw_http_server_callbacks[2];

static int http_onReceive(swFactory *factory, swEventData *req);
static void http_onClose(swServer *serv, int fd, int from_id);

static int http_request_on_path(php_http_parser *parser, const char *at, size_t length);
static int http_request_on_query_string(php_http_parser *parser, const char *at, size_t length);
static int http_request_on_body(php_http_parser *parser, const char *at, size_t length);
static int http_request_on_header_field(php_http_parser *parser, const char *at, size_t length);
static int http_request_on_header_value(php_http_parser *parser, const char *at, size_t length);
static int http_request_on_headers_complete(php_http_parser *parser);
static int http_request_message_complete(php_http_parser *parser);

static int http_request_new(http_client* c TSRMLS_DC);

static void http_global_merge(zval *val, zval *zrequest, int type);
static void http_global_clear(TSRMLS_D);
static http_client *http_get_client(zval *object TSRMLS_DC);
static void http_build_header(http_client *client, zval *object, swString *response, int body_length TSRMLS_DC);

#define http_merge_php_global(v,r,t)  if (http_merge_global_flag > 0) http_global_merge(v,r,t)

static sw_inline char* http_get_method_name(int method)
{
    switch (method)
    {
    case PHP_HTTP_GET:
        return "GET";
    case PHP_HTTP_POST:
        return "POST";
    case PHP_HTTP_HEAD:
        return "HEAD";
    case PHP_HTTP_PUT:
        return "PUT";
    case PHP_HTTP_DELETE:
        return "DELETE";
    case PHP_HTTP_PATCH:
        return "PATCH";
    case PHP_HTTP_CONNECT:
        return "CONNECT";
    case PHP_HTTP_OPTIONS:
        return "OPTIONS";
    case PHP_HTTP_TRACE:
        return "TRACE";
    case PHP_HTTP_COPY:
        return "COPY";
    case PHP_HTTP_LOCK:
        return "LOCK";
    case PHP_HTTP_MKCOL:
        return "MKCOL";
    case PHP_HTTP_MOVE:
        return "MOVE";
    case PHP_HTTP_PROPFIND:
        return "PROPFIND";
    case PHP_HTTP_PROPPATCH:
        return "PROPPATCH";
    case PHP_HTTP_UNLOCK:
        return "UNLOCK";
        /* subversion */
    case PHP_HTTP_REPORT:
        return "REPORT";
    case PHP_HTTP_MKACTIVITY:
        return "MKACTIVITY";
    case PHP_HTTP_CHECKOUT:
        return "CHECKOUT";
    case PHP_HTTP_MERGE:
        return "MERGE";
        /* upnp */
    case PHP_HTTP_MSEARCH:
        return "MSEARCH";
    case PHP_HTTP_NOTIFY:
        return "NOTIFY";
    case PHP_HTTP_SUBSCRIBE:
        return "SUBSCRIBE";
    case PHP_HTTP_UNSUBSCRIBE:
        return "UNSUBSCRIBE";
    case PHP_HTTP_NOT_IMPLEMENTED:
        return "IMPLEMENTED";
    default:
        return NULL;
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_server_on, 0, 0, 2)
    ZEND_ARG_INFO(0, ha_name)
    ZEND_ARG_INFO(0, cb)
ZEND_END_ARG_INFO()

static const php_http_parser_settings http_parser_settings =
{
    NULL,
    http_request_on_path,
    http_request_on_query_string,
    NULL,
    NULL,
    http_request_on_header_field,
    http_request_on_header_value,
    http_request_on_headers_complete,
    http_request_on_body,
    http_request_message_complete
};

const zend_function_entry swoole_http_server_methods[] =
{
    PHP_ME(swoole_http_server, on,         arginfo_swoole_http_server_on, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server, setglobal,  NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server, start,      NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const zend_function_entry swoole_http_request_methods[] =
{
    PHP_ME(swoole_http_request, rawcontent,         NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const zend_function_entry swoole_http_response_methods[] =
{
    PHP_ME(swoole_http_response, cookie, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, rawcookie, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, status, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, header, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, write, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_response, end, NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static int http_request_on_path(php_http_parser *parser, const char *at, size_t length)
{
    http_client *client = parser->data;
    client->request.path = estrndup(at, length);
    client->request.path_len = length;
    return 0;
}

static void http_global_clear(TSRMLS_D)
{
    zend_hash_del(&EG(symbol_table), "_GET", sizeof("_GET"));
    zend_hash_del(&EG(symbol_table), "_POST", sizeof("_POST"));
    zend_hash_del(&EG(symbol_table), "_COOKIE", sizeof("_COOKIE"));
    zend_hash_del(&EG(symbol_table), "_REQUEST", sizeof("_REQUEST"));
    zend_hash_del(&EG(symbol_table), "_SERVER", sizeof("_SERVER"));
}

static void http_global_merge(zval *val, zval *zrequest, int type)
{
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
    zval *_request;

    if (type == HTTP_GLOBAL_SERVER)
    {
        zval *php_global_server;
        MAKE_STD_ZVAL(php_global_server);
        array_init(php_global_server);

        char *key;
        char _php_key[128];
        int keytype;
        uint32_t keylen;
        ulong idx;
        zval **value;

        zval *server = zend_read_property(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("server"), 1 TSRMLS_CC);
        if (server || !ZVAL_IS_NULL(server))
        {
            for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(server));
                zend_hash_has_more_elements(Z_ARRVAL_P(server)) == SUCCESS;
                zend_hash_move_forward(Z_ARRVAL_P(server)))
            {
                keytype = zend_hash_get_current_key_ex(Z_ARRVAL_P(server), &key, &keylen, &idx, 0, NULL);
                if (HASH_KEY_IS_STRING != keytype)
                {
                    continue;
                }
                if (zend_hash_get_current_data(Z_ARRVAL_P(server), (void**)&value) == FAILURE)
                {
                    continue;
                }
                strncpy(_php_key, key, sizeof(_php_key));
                php_strtoupper(_php_key, keylen);
                convert_to_string(*value);
                add_assoc_stringl_ex(php_global_server, _php_key, keylen, Z_STRVAL_PP(value), Z_STRLEN_PP(value), 1);
            }
        }

        zval *header = zend_read_property(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("header"), 1 TSRMLS_CC);
        if (header || !ZVAL_IS_NULL(header))
        {
            for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(header));
                zend_hash_has_more_elements(Z_ARRVAL_P(header)) == SUCCESS;
                zend_hash_move_forward(Z_ARRVAL_P(header)))
            {
                keytype = zend_hash_get_current_key_ex(Z_ARRVAL_P(header), &key, &keylen, &idx, 0, NULL);
                if (HASH_KEY_IS_STRING != keytype)
                {
                    continue;
                }
                if (zend_hash_get_current_data(Z_ARRVAL_P(header), (void**)&value) == FAILURE)
                {
                    continue;
                }
                int i;
                //replace '-' to '_'
                for (i = 0; i < keylen; i++)
                {
                    if (key[i] == '-')
                    {
                        key[i] = '_';
                    }
                }
                keylen = snprintf(_php_key, sizeof(_php_key), "HTTP_%s", key) + 1;
                php_strtoupper(_php_key, keylen);
                convert_to_string(*value);
                add_assoc_stringl_ex(php_global_server, _php_key, keylen, Z_STRVAL_PP(value), Z_STRLEN_PP(value), 1);
            }
        }
        ZEND_SET_SYMBOL(&EG(symbol_table), "_SERVER", php_global_server);
        return;
    }

    switch (type)
    {
    case HTTP_GLOBAL_GET:
        ZEND_SET_SYMBOL(&EG(symbol_table), "_GET", val);
        break;

    case HTTP_GLOBAL_POST:
        ZEND_SET_SYMBOL(&EG(symbol_table), "_POST", val);
        break;

    case HTTP_GLOBAL_COOKIE:
        ZEND_SET_SYMBOL(&EG(symbol_table), "_COOKIE", val);
        break;

    case HTTP_GLOBAL_REQUEST:
        if (!http_merge_request_flag)
        {
            return;
        }
        _request = zend_read_property(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("request"), 1 TSRMLS_CC);
        if (_request && !(ZVAL_IS_NULL(_request)))
        {
            ZEND_SET_SYMBOL(&EG(symbol_table), "_REQUEST", _request);
        }
        return;

    default:
        swWarn("unknow global type [%d]", type);
        return;
    }

    if (http_merge_request_flag & type)
    {
        _request = zend_read_property(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("request"), 1 TSRMLS_CC);
        if (!_request || ZVAL_IS_NULL(_request))
        {
            _request = val;
        }
        else
        {
            zend_hash_copy(Z_ARRVAL_P(_request), Z_ARRVAL_P(val), NULL, NULL, sizeof(zval));
        }
        zend_update_property(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("request"), _request TSRMLS_CC);
    }
}

static int http_request_on_query_string(php_http_parser *parser, const char *at, size_t length)
{
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

    http_client *client = parser->data;

    //no need free, will free by treat_data
    char *query = estrndup(at, length);

    zval *get;
    MAKE_STD_ZVAL(get);
    array_init(get);
    zend_update_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("get"), get TSRMLS_CC);
    sapi_module.treat_data(PARSE_STRING, query, get TSRMLS_CC);

    http_merge_php_global(get, client->zrequest, HTTP_GLOBAL_GET);

    return 0;
}

static int http_request_on_header_field(php_http_parser *parser, const char *at, size_t length)
{
    http_client *client = parser->data;
    if (client->current_header_name_allocated)
    {
        efree(client->current_header_name);
        client->current_header_name_allocated = 0;
    }
    client->current_header_name = (char *)at;
    client->current_header_name_len = length;
    return 0;
}

static int http_request_on_header_value(php_http_parser *parser, const char *at, size_t length)
{
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

    http_client *client = parser->data;
    char *header_name = zend_str_tolower_dup(client->current_header_name, client->current_header_name_len);
    char keybuf[SW_HTTP_COOKIE_KEYLEN];

    if (memcmp(header_name, ZEND_STRL("cookie")) == 0)
    {
        zval *cookie;
        MAKE_STD_ZVAL(cookie);
        array_init(cookie);
        zend_update_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("cookie"), cookie TSRMLS_CC);

        struct
        {
            char *k;
            int klen;
            char *v;
            int vlen;
        } kv = { 0 };

        char *_c = (char *) at;
        int n = 1;
        kv.k = _c;

        while (_c < at + length)
        {
            if (*_c == '=')
            {
                kv.v = _c + 1;
                kv.klen = n;
                n = 0;
            }
            else if (*_c == ';')
            {
                kv.vlen = n;
                if (kv.klen >= SW_HTTP_COOKIE_KEYLEN)
                {
                    kv.klen = SW_HTTP_COOKIE_KEYLEN - 1;
                }
                memcpy(keybuf, kv.k, kv.klen - 1);
                keybuf[kv.klen - 1] = 0;
                add_assoc_stringl_ex(cookie, keybuf, kv.klen, kv.v, kv.vlen, 1);
                kv.k = _c + 2;
                n = 0;
            }
            else
            {
                n++;
            }
            _c++;
        }
        kv.vlen = n;
        if (kv.klen >= SW_HTTP_COOKIE_KEYLEN)
        {
            kv.klen = SW_HTTP_COOKIE_KEYLEN - 1;
        }
        memcpy(keybuf, kv.k, kv.klen - 1);
        keybuf[kv.klen - 1] = 0;
        add_assoc_stringl_ex(cookie, keybuf, kv.klen , kv.v, kv.vlen, 1);
        http_merge_php_global(cookie, client->zrequest, HTTP_GLOBAL_COOKIE);
    }
    else if (strncasecmp(header_name, ZEND_STRL("upgrade")) == 0 && strncasecmp(at, ZEND_STRL("websocket")) == 0)
    {
        swConnection *conn = swWorker_get_connection(SwooleG.serv, client->fd);
        if (!conn)
        {
            swWarn("connection[%d] is closed.", client->fd);
            return SW_ERR;
        }
        conn->websocket_status = WEBSOCKET_STATUS_CONNECTION;
        zval *header = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("header"), 1 TSRMLS_CC);
        add_assoc_stringl_ex(header, header_name, client->current_header_name_len + 1, (char *) at, length, 1);
    }
    else if ((parser->method == PHP_HTTP_POST || parser->method == PHP_HTTP_PUT || parser->method == PHP_HTTP_PATCH)
            && memcmp(header_name, ZEND_STRL("content-type")) == 0
            && strncasecmp(at, ZEND_STRL("application/x-www-form-urlencoded")) == 0)
    {
        client->request.post_form_urlencoded = 1;
        zval *header = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("header"), 1 TSRMLS_CC);
        add_assoc_stringl_ex(header, header_name, client->current_header_name_len + 1, (char *) at, length, 1);
    }
    else
    {
        zval *header = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("header"), 1
        TSRMLS_CC);
        add_assoc_stringl_ex(header, header_name, client->current_header_name_len + 1, (char *) at, length, 1);
    }

    if (client->current_header_name_allocated)
    {
        efree(client->current_header_name);
        client->current_header_name_allocated = 0;
    }
    efree(header_name);
    return 0;
}

static int http_request_on_headers_complete(php_http_parser *parser)
{
    http_client *client = parser->data;
    if (client->current_header_name_allocated)
    {
        efree(client->current_header_name);
        client->current_header_name_allocated = 0;
    }
    client->current_header_name = NULL;
    return 0;
}

static int http_request_on_body(php_http_parser *parser, const char *at, size_t length)
{
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

    http_client *client = parser->data;
    char *body = estrndup(at, length);

    if (client->request.post_form_urlencoded)
    {
        zval *post;
        MAKE_STD_ZVAL(post);
        array_init(post);
        zend_update_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("post"), post TSRMLS_CC);
        sapi_module.treat_data(PARSE_STRING, body, post TSRMLS_CC);
        http_merge_php_global(post, client->zrequest, HTTP_GLOBAL_POST);
    }
    else
    {
        client->request.post_content = body;
        client->request.post_length = length;
    }

    return 0;
}

static int http_request_message_complete(php_http_parser *parser)
{
    http_client *client = parser->data;
    client->request.version = parser->http_major * 100 + parser->http_minor;

    const char *vpath = client->request.path, *end = vpath + client->request.path_len, *p = end;
    client->request.ext = end;
    client->request.ext_len = 0;
    while (p > vpath)
    {
        --p;
        if (*p == '.')
        {
            ++p;
            client->request.ext = p;
            client->request.ext_len = end - p;
            break;
        }
    }
    client->request_read = 1;
    return 0;
}

static void http_onClose(swServer *serv, int fd, int from_id)
{
    http_client *client = swArray_fetch(http_client_array, fd);
    if (client)
    {
        if (client->zrequest && !client->end)
        {
            TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
            swoole_http_request_free(client TSRMLS_CC);
        }
    }

    if (php_sw_callback[SW_SERVER_CB_onClose] != NULL)
    {
        php_swoole_onClose(serv, fd, from_id);
    }
}

static int http_onReceive(swFactory *factory, swEventData *req)
{
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

    int fd = req->info.fd;

    swConnection *conn = swWorker_get_connection(SwooleG.serv, fd);
    if (!conn)
    {
        swWarn("connection[%d] is closed.", fd);
        return SW_ERR;
    }

    if (conn->websocket_status == WEBSOCKET_STATUS_FRAME)  //websocket callback
    {
        return swoole_websocket_onMessage(req);
    }

    http_client *client = swArray_alloc(http_client_array, fd);
    if (!client)
    {
        return SW_OK;
    }
    client->fd = fd;

    php_http_parser *parser = &client->parser;

    /**
     * create request and response object
     */
    http_request_new(client TSRMLS_CC);

    parser->data = client;

    php_http_parser_init(parser, PHP_HTTP_REQUEST);

    zval *zdata = php_swoole_get_data(req TSRMLS_CC);

    swTrace("httpRequest %d bytes:\n---------------------------------------\n%s\n", Z_STRLEN_P(zdata), Z_STRVAL_P(zdata));

    long n = php_http_parser_execute(parser, &http_parser_settings, Z_STRVAL_P(zdata), Z_STRLEN_P(zdata));
    zval_ptr_dtor(&zdata);

    if (n < 0)
    {
        swWarn("php_http_parser_execute failed.");
        if (conn->websocket_status == WEBSOCKET_STATUS_CONNECTION)
        {
            return SwooleG.serv->factory.end(&SwooleG.serv->factory, fd);
        }
    }
    else
    {
        //websocket handshake
        if (conn->websocket_status == WEBSOCKET_STATUS_CONNECTION && php_sw_http_server_callbacks[1] == NULL)
        {
            return swoole_websocket_onHandshake(client);
        }

        zval *retval;
        zval **args[2];
        zval *zrequest = client->zrequest;

    	//server info
    	zval *zserver;
    	MAKE_STD_ZVAL(zserver);

    	array_init(zserver);
    	zend_update_property(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("server"), zserver TSRMLS_CC);

    	char *method_name = http_get_method_name(parser->method);

        add_assoc_string(zserver, "request_method", method_name, 1);
        add_assoc_stringl(zserver, "request_uri", client->request.path, client->request.path_len, 1);
        add_assoc_stringl(zserver, "path_info", client->request.path, client->request.path_len, 1);
        add_assoc_long_ex(zserver, ZEND_STRS("request_time"), SwooleGS->now);

    	swConnection *conn = swWorker_get_connection(SwooleG.serv, fd);
        if (!conn)
        {
            swWarn("connection[%d] is closed.", fd);
            return SW_ERR;
        }

        add_assoc_long(zserver, "server_port", swConnection_get_port(&SwooleG.serv->connection_list[conn->from_fd]));
        add_assoc_long(zserver, "remote_port", swConnection_get_port(conn));
        add_assoc_string(zserver, "remote_addr", swConnection_get_ip(conn), 1);

        if (client->request.version == 101)
        {
            add_assoc_string(zserver, "server_protocol", "HTTP/1.1", 1);
        }
        else
        {
            add_assoc_string(zserver, "server_protocol", "HTTP/1.0", 1);
        }

        add_assoc_string(zserver, "server_software", SW_HTTP_SERVER_SOFTWARE, 1);

        http_merge_php_global(NULL, zrequest, HTTP_GLOBAL_SERVER);
        http_merge_php_global(NULL, zrequest, HTTP_GLOBAL_REQUEST);

    	zval *zresponse;
    	MAKE_STD_ZVAL(zresponse);
    	object_init_ex(zresponse, swoole_http_response_class_entry_ptr);

    	//socket fd
    	zend_update_property_long(swoole_http_response_class_entry_ptr, zresponse, ZEND_STRL("fd"), client->fd TSRMLS_CC);
    	client->zresponse = zresponse;      

#ifdef __CYGWIN__
        //TODO: memory error on cygwin.
        zval_add_ref(&zrequest);
        zval_add_ref(&zresponse);
#endif
        
        args[0] = &zrequest;
        args[1] = &zresponse;

        int called = 0;
        if (conn->websocket_status == WEBSOCKET_STATUS_CONNECTION)
        {
            called = 1;
        }
        if (call_user_function_ex(EG(function_table), NULL, php_sw_http_server_callbacks[called], &retval, 2, args, 0, NULL TSRMLS_CC) == FAILURE)
        {
            php_error_docref(NULL TSRMLS_CC, E_WARNING, "onRequest handler error");
        }
        if (EG(exception))
        {
            zend_exception_error(EG(exception), E_ERROR TSRMLS_CC);
        }
        if (retval)
        {
            zval_ptr_dtor(&retval);
        }
        swTrace("======call end======\n");
        if (called == 1)
        {
            swoole_websocket_onOpen(client->fd);
        }
    }
    return SW_OK;
}

void swoole_http_init(int module_number TSRMLS_DC)
{
    INIT_CLASS_ENTRY(swoole_http_server_ce, "swoole_http_server", swoole_http_server_methods);
    swoole_http_server_class_entry_ptr = zend_register_internal_class_ex(&swoole_http_server_ce, swoole_server_class_entry_ptr, "swoole_server" TSRMLS_CC);

    zend_declare_property_long(swoole_http_server_class_entry_ptr, ZEND_STRL("global"), 0, ZEND_ACC_PRIVATE  TSRMLS_CC);

    INIT_CLASS_ENTRY(swoole_http_response_ce, "swoole_http_response", swoole_http_response_methods);
    swoole_http_response_class_entry_ptr = zend_register_internal_class(&swoole_http_response_ce TSRMLS_CC);

    INIT_CLASS_ENTRY(swoole_http_request_ce, "swoole_http_request", swoole_http_request_methods);
    swoole_http_request_class_entry_ptr = zend_register_internal_class(&swoole_http_request_ce TSRMLS_CC);
    
    REGISTER_LONG_CONSTANT("HTTP_GLOBAL_GET", HTTP_GLOBAL_GET, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("HTTP_GLOBAL_POST", HTTP_GLOBAL_POST, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("HTTP_GLOBAL_COOKIE", HTTP_GLOBAL_COOKIE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("HTTP_GLOBAL_ALL", HTTP_GLOBAL_GET| HTTP_GLOBAL_POST| HTTP_GLOBAL_COOKIE | HTTP_GLOBAL_REQUEST |HTTP_GLOBAL_SERVER, CONST_CS | CONST_PERSISTENT);
}

PHP_METHOD(swoole_http_server, on)
{
    zval *callback;
    zval *event_name;
    swServer *serv;

    if (SwooleGS->start > 0)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Server is running. Unable to set event callback now.");
        RETURN_FALSE;
    }

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &event_name, &callback) == FAILURE)
    {
        return;
    }

    SWOOLE_GET_SERVER(getThis(), serv);

    char *func_name = NULL;
    if (!zend_is_callable(callback, 0, &func_name TSRMLS_CC))
    {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "Function '%s' is not callable", func_name);
        efree(func_name);
        RETURN_FALSE;
    }
    efree(func_name);

    if (strncasecmp("request", Z_STRVAL_P(event_name), Z_STRLEN_P(event_name)) == 0)
    {
        zval_add_ref(&callback);
        php_sw_http_server_callbacks[0] = callback;
    }
    else if (strncasecmp("handshake", Z_STRVAL_P(event_name), Z_STRLEN_P(event_name)) == 0)
    {
        zval_add_ref(&callback);
        php_sw_http_server_callbacks[1] = callback;
    }
    else
    {
        zend_call_method_with_2_params(&getThis(), swoole_server_class_entry_ptr, NULL, "on", &return_value, event_name, callback);
    }
}

static int http_request_new(http_client* client TSRMLS_DC)
{
	zval *zrequest;
	MAKE_STD_ZVAL(zrequest);
	object_init_ex(zrequest, swoole_http_request_class_entry_ptr);

	//http header
	zval *header;
	MAKE_STD_ZVAL(header);
	array_init(header);
	zend_update_property(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("header"), header TSRMLS_CC);

	client->zrequest = zrequest;
	client->end = 0;

	zend_update_property_long(swoole_http_request_class_entry_ptr, zrequest, ZEND_STRL("fd"), client->fd TSRMLS_CC);

	bzero(&client->request, sizeof(client->request));
	bzero(&client->response, sizeof(client->response));

	return SW_OK;
}

void swoole_http_request_free(http_client *client TSRMLS_DC)
{
    http_request *req = &client->request;
    if (req->path)
    {
        efree(req->path);
    }
    if (req->post_content)
    {
        efree(req->post_content);
    }
    http_response *resp = &client->response;
    if (resp->cookie)
    {
        swString_free(resp->cookie);
    }
    /**
     * Free request object
     */
    zval *zheader = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("header"), 1 TSRMLS_CC);
    if (!ZVAL_IS_NULL(zheader))
    {
        zval_ptr_dtor(&zheader);
    }
    zval *zget = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("get"), 1 TSRMLS_CC);
    if (!ZVAL_IS_NULL(zget))
    {
        zval_ptr_dtor(&zget);
    }
    zval *zpost = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("post"), 1 TSRMLS_CC);
    if (!ZVAL_IS_NULL(zpost))
    {
        zval_ptr_dtor(&zpost);
    }
    zval *zcookie = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("cookie"), 1 TSRMLS_CC);
    if (!ZVAL_IS_NULL(zcookie))
    {
        zval_ptr_dtor(&zcookie);
    }
    zval *zrequest = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("request"), 1 TSRMLS_CC);
    if (!ZVAL_IS_NULL(zrequest))
    {
        zval_ptr_dtor(&zrequest);
    }
    zval *zserver = zend_read_property(swoole_http_request_class_entry_ptr, client->zrequest, ZEND_STRL("server"), 1 TSRMLS_CC);
    if (!ZVAL_IS_NULL(zserver))
    {
        zval_ptr_dtor(&zserver);
    }
    zval_ptr_dtor(&client->zrequest);
    client->zrequest = NULL;

    if (client->zresponse)
    {
        zval_ptr_dtor(&client->zresponse);
        client->zresponse = NULL;
    }
    client->end = 1;
}

static char *http_status_message(int code)
{
    switch (code)
    {
    case 100:
        return "100 Continue";
    case 101:
        return "101 Switching Protocols";
    case 201:
        return "201 Created";
    case 204:
        return "204 No Content";
    case 206:
        return "206 Partial Content";
    case 300:
        return "300 Multiple Choices";
    case 301:
        return "301 Moved Permanently";
    case 302:
        return "302 Found";
    case 303:
        return "303 See Other";
    case 304:
        return "304 Not Modified";
    case 307:
        return "307 Temporary Redirect";
    case 400:
        return "400 Bad Request";
    case 401:
        return "401 Unauthorized";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 406:
        return "406 Not Acceptable";
    case 408:
        return "408 Request Timeout";
    case 410:
        return "410 Gone";
    case 413:
        return "413 Request Entity Too Large";
    case 414:
        return "414 Request URI Too Long";
    case 415:
        return "415 Unsupported Media Type";
    case 416:
        return "416 Requested Range Not Satisfiable";
    case 417:
        return "417 Expectation Failed";
    case 500:
        return "500 Internal Server Error";
    case 501:
        return "501 Method Not Implemented";
    case 503:
        return "503 Service Unavailable";
    case 506:
        return "506 Variant Also Negotiates";
    case 200:
    default:
        return "200 OK";
    }
}

PHP_METHOD(swoole_http_server, setglobal)
{
    long global_flag = 0;
    long request_flag = HTTP_GLOBAL_GET | HTTP_GLOBAL_POST;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|l", &global_flag, &request_flag) == FAILURE)
    {
        return;
    }

    http_merge_global_flag = global_flag;
    http_merge_request_flag = request_flag;

    RETURN_TRUE;
}

PHP_METHOD(swoole_http_server, start)
{
    swServer *serv;
    int ret;

    if (SwooleGS->start > 0)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Server is running. Unable to execute swoole_server::start.");
        RETURN_FALSE;
    }

    SWOOLE_GET_SERVER(getThis(), serv);
    php_swoole_register_callback(serv);

    if (serv->open_websocket_protocol)
    {
        if (!swoole_websocket_isset_onMessage())
        {
            swoole_php_fatal_error(E_ERROR, "require onMessage callback");
            RETURN_FALSE;
        }
    }
    else if (php_sw_http_server_callbacks[0] == NULL)
    {
        swoole_php_fatal_error(E_ERROR, "require onRequest callback");
        RETURN_FALSE;
    }

    http_client_array = swArray_new(1024, sizeof(http_client), 0);
    if (!http_client_array)
    {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "swArray_new failed.");
        RETURN_FALSE;
    }

    serv->onReceive = http_onReceive;
    serv->onClose = http_onClose;
    serv->open_http_protocol = 1;

    serv->ptr2 = getThis();

    ret = swServer_create(serv);
    if (ret < 0)
    {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "create server failed. Error: %s", sw_error);
        RETURN_LONG(ret);
    }
    zend_update_property_long(swoole_server_class_entry_ptr, getThis(), ZEND_STRL("master_pid"), getpid() TSRMLS_CC);
    ret = swServer_start(serv);
    if (ret < 0)
    {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "start server failed. Error: %s", sw_error);
        RETURN_LONG(ret);
    }
    RETURN_TRUE;
}

PHP_METHOD(swoole_http_request, rawcontent)
{
    zval *zfd = zend_read_property(swoole_http_request_class_entry_ptr, getThis(), ZEND_STRL("fd"), 0 TSRMLS_CC);
    if (ZVAL_IS_NULL(zfd))
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client not exists.");
        RETURN_FALSE;
    }
    http_client *client = swArray_fetch(http_client_array, Z_LVAL_P(zfd));
    if (!client)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client[#%d] not exists.", (int) Z_LVAL_P(zfd));
        RETURN_FALSE;
    }
    if (!client->request.post_content)
    {
        RETURN_FALSE;
    }
    RETVAL_STRINGL(client->request.post_content, client->request.post_length, 0);
    client->request.post_content = NULL;
}

PHP_METHOD(swoole_http_response, write)
{
    swString body;
    body.length = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &body.str, &body.length) == FAILURE)
    {
        return;
    }

    http_client *client = http_get_client(getThis() TSRMLS_CC);
    if (!client)
    {
        return;
    }

    if (!client->send_header)
    {
        client->chunk = 1;
        swString *buffer = swString_new(SW_HTTP_HEADER_INIT_SIZE);
        http_build_header(client, getThis(), buffer, 0 TSRMLS_CC);
        swServer_tcp_send(SwooleG.serv, client->fd, buffer->str, buffer->length);
        swString_free(buffer);
    }

    char stack_buf[4096];
    char *hex_string = swoole_dec2hex(body.length, 16);
    int hex_len = strlen(hex_string);

    char *buf;
    int free_buf = 0;
    size_t buf_size;

    if (body.length < sizeof(stack_buf) - 8)
    {
        buf = stack_buf;
        buf_size = sizeof(stack_buf);
    }
    else
    {
        buf_size = body.length + hex_len + (2 * sizeof("\r\n") + sizeof('\n'));
        buf = emalloc(buf_size);
        free_buf = 1;
    }

    int n = snprintf(buf, buf_size, "%*s\r\n%*s\r\n", hex_len, hex_string, body.length, body.str);
    int ret = swServer_tcp_send(SwooleG.serv, client->fd, buf, n);

    if (free_buf)
    {
        efree(buf);
    }
    free(hex_string);

    SW_CHECK_RETURN(ret);
}

static http_client *http_get_client(zval *object TSRMLS_DC)
{
    zval *zfd = zend_read_property(swoole_http_response_class_entry_ptr, object, ZEND_STRL("fd"), 0 TSRMLS_CC);
    if (ZVAL_IS_NULL(zfd))
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client not exists.");
        return NULL;
    }

    http_client *client = swArray_fetch(http_client_array, Z_LVAL_P(zfd));
    if (!client)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client[#%d] not exists.", (int) Z_LVAL_P(zfd));
        return NULL;
    }

    if (client->end)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Response is end.");
        return NULL;
    }

    return client;
}

static void http_build_header(http_client *client, zval *object, swString *response, int body_length TSRMLS_DC)
{
    assert(client->send_header == 0);

    char buf[128];
    int n;
    char *date_str;

    client->keepalive = php_http_should_keep_alive(&client->parser);

    /**
     * http status line
     */
    n = snprintf(buf, sizeof(buf), "HTTP/1.1 %s\r\n", http_status_message(client->response.status));
    swString_append_ptr(response, buf, n);

    /**
     * http header
     */
    zval *header =  zend_read_property(swoole_http_response_class_entry_ptr, object, ZEND_STRL("header"), 1 TSRMLS_CC);

    if (!ZVAL_IS_NULL(header))
    {
        int flag = 0x0;
        char *key_server = "Server";
        char *key_connection = "Connection";
        char *key_content_length = "Content-Length";
        char *key_content_type = "Content-Type";
        char *key_date = "Date";

        HashTable *ht = Z_ARRVAL_P(header);
        for (zend_hash_internal_pointer_reset(ht); zend_hash_has_more_elements(ht) == 0; zend_hash_move_forward(ht))
        {
            char *key;
            uint keylen;
            ulong idx;
            int type;
            zval **value;

            type = zend_hash_get_current_key_ex(ht, &key, &keylen, &idx, 0, NULL);
            if (type == HASH_KEY_IS_LONG || zend_hash_get_current_data(ht, (void**)&value) == FAILURE)
            {
                continue;
            }
            if (strcmp(key, key_server) == 0)
            {
                flag |= HTTP_RESPONSE_SERVER;
            }
            else if (strcmp(key, key_connection) == 0)
            {
                flag |= HTTP_RESPONSE_CONNECTION;
            }
            else if (strcmp(key, key_content_length) == 0)
            {
                flag |= HTTP_RESPONSE_CONTENT_LENGTH;
            }
            else if (strcmp(key, key_date) == 0)
            {
                flag |= HTTP_RESPONSE_DATE;
            }
            else if (strcmp(key, key_content_type) == 0)
            {
                flag |= HTTP_RESPONSE_CONTENT_TYPE;
            }
            n = snprintf(buf, sizeof(buf), "%*s: %*s\r\n", keylen - 1, key, Z_STRLEN_PP(value), Z_STRVAL_PP(value));
            swString_append_ptr(response, buf, n);
        }
        if (!(flag & HTTP_RESPONSE_SERVER))
        {
            swString_append_ptr(response, ZEND_STRL("Server: "SW_HTTP_SERVER_SOFTWARE"\r\n"));
        }
        if (!(flag & HTTP_RESPONSE_CONNECTION))
        {
            if (client->keepalive)
            {
                swString_append_ptr(response, ZEND_STRL("Connection: keep-alive\r\n"));
            }
            else
            {
                swString_append_ptr(response, ZEND_STRL("Connection: close\r\n"));
            }
        }
        if (client->request.method == PHP_HTTP_OPTIONS)
        {
            swString_append_ptr(response, ZEND_STRL("Allow: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS\r\nContent-Length: 0\r\n"));
        }
        else
        {
            if (!(flag & HTTP_RESPONSE_CONTENT_LENGTH) && body_length > 0)
            {
                n = snprintf(buf, sizeof(buf), "Content-Length: %d\r\n", body_length);
                swString_append_ptr(response, buf, n);
            }
        }
        if (!(flag & HTTP_RESPONSE_DATE))
        {
            date_str = php_format_date(ZEND_STRL("D, d-M-Y H:i:s T"), SwooleGS->now, 0 TSRMLS_CC);
            n = snprintf(buf, sizeof(buf), "Date: %s\r\n", date_str);
            swString_append_ptr(response, buf, n);
            efree(date_str);
        }
        if (!(flag & HTTP_RESPONSE_CONTENT_TYPE))
        {
            swString_append_ptr(response, ZEND_STRL("Content-Type: text/html\r\n"));
        }
    }
    else
    {
        swString_append_ptr(response, ZEND_STRL("Server: "SW_HTTP_SERVER_SOFTWARE"\r\nContent-Type: text/html\r\n"));
        if (client->keepalive)
        {
            swString_append_ptr(response, ZEND_STRL("Connection: keep-alive\r\n"));
        }
        else
        {
            swString_append_ptr(response, ZEND_STRL("Connection: close\r\n"));
        }

        date_str = php_format_date(ZEND_STRL("D, d-M-Y H:i:s T"), SwooleGS->now, 0 TSRMLS_CC);
        n = snprintf(buf, sizeof(buf), "Date: %s\r\n", date_str);
        efree(date_str);
        swString_append_ptr(response, buf, n);

        if (client->request.method == PHP_HTTP_OPTIONS)
        {
            n = snprintf(buf, sizeof(buf), "Allow: GET, POST, PUT, DELETE, HEAD, OPTIONS\r\nContent-Length: %d\r\n", 0);
            swString_append_ptr(response, buf, n);
        }
        else if (body_length > 0)
        {
            n = snprintf(buf, sizeof(buf), "Content-Length: %d\r\n", body_length);
            swString_append_ptr(response, buf, n);
        }
    }

    if (client->chunk)
    {
        swString_append_ptr(response, SW_STRL("Transfer-Encoding: chunked\r\n") - 1);
    }

    //http cookies
    if (client->response.cookie)
    {
        swString_append(response, client->response.cookie);
    }

    swString_append_ptr(response, ZEND_STRL("\r\n"));
    client->send_header = 1;
}

PHP_METHOD(swoole_http_response, end)
{
    int ret;
    swString body;
    body.length = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &body.str, &body.length) == FAILURE)
    {
        return;
    }

    http_client *client = http_get_client(getThis() TSRMLS_CC);
    if (!client)
    {
        return;
    }

    if (client->chunk)
    {
        ret = swServer_tcp_send(SwooleG.serv, client->fd, SW_STRL("0\r\n\r\n") - 1);
        client->chunk = 0;
    }
    //no http chunk
    else
    {
        swString *response = swString_new(body.length + SW_HTTP_HEADER_INIT_SIZE);
        http_build_header(client, getThis(), response, body.length TSRMLS_CC);

        if (client->request.method != PHP_HTTP_HEAD && body.length > 0)
        {
            swString_append(response, &body);
        }

        ret = swServer_tcp_send(SwooleG.serv, client->fd, response->str, response->length);
        swString_free(response);
    }

    swoole_http_request_free(client TSRMLS_CC);
    client->send_header = 0;

    if (!client->keepalive)
    {
        SwooleG.serv->factory.end(&SwooleG.serv->factory, client->fd);
    }
    if (http_merge_global_flag > 0)
    {
        http_global_clear(TSRMLS_C);
    }
    SW_CHECK_RETURN(ret);
}

PHP_METHOD(swoole_http_response, cookie)
{
    char *name, *value = NULL, *path = NULL, *domain = NULL;
    long expires = 0;
    int encode = 1;
    zend_bool secure = 0, httponly = 0;
    int name_len, value_len = 0, path_len = 0, domain_len = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|slssbb", &name, &name_len, &value, &value_len, &expires,
                &path, &path_len, &domain, &domain_len, &secure, &httponly) == FAILURE)
    {
        return;
    }

    zval *zfd = zend_read_property(swoole_http_response_class_entry_ptr, getThis(), ZEND_STRL("fd"), 0 TSRMLS_CC);
    if (ZVAL_IS_NULL(zfd))
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client not exists.");
        RETURN_FALSE;
    }

    http_client *client = swArray_fetch(http_client_array, Z_LVAL_P(zfd));
    if (!client)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client[#%d] not exists.", (int) Z_LVAL_P(zfd));
        RETURN_FALSE;
    }

    if (client->end)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Response is end.");
        RETURN_FALSE;
    }

    char *cookie, *encoded_value = NULL;
    int len = sizeof("Set-Cookie: ");
    char *dt;

    if (name && strpbrk(name, "=,; \t\r\n\013\014") != NULL)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cookie names cannot contain any of the following '=,; \\t\\r\\n\\013\\014'");
        RETURN_FALSE;
    }

    if (!client->response.cookie)
    {
        client->response.cookie = swString_new(1024);
    }

    len += name_len;
    if (encode && value)
    {
        int encoded_value_len;
        encoded_value = php_url_encode(value, value_len, &encoded_value_len);
        len += encoded_value_len;
    }
    else if (value)
    {
        encoded_value = estrdup(value);
        len += value_len;
    }
    if (path)
    {
        len += path_len;
    }
    if (domain)
    {
        len += domain_len;
    }

    cookie = emalloc(len + 100);

    if (value && value_len == 0)
    {
        dt = php_format_date("D, d-M-Y H:i:s T", sizeof("D, d-M-Y H:i:s T") - 1, 1, 0 TSRMLS_CC);
        snprintf(cookie, len + 100, "Set-Cookie: %s=deleted; expires=%s", name, dt);
        efree(dt);
    }
    else
    {
        snprintf(cookie, len + 100, "Set-Cookie: %s=%s", name, value ? encoded_value : "");
        if (expires > 0)
        {
            const char *p;
            strlcat(cookie, "; expires=", len + 100);
            dt = php_format_date("D, d-M-Y H:i:s T", sizeof("D, d-M-Y H:i:s T") - 1, expires, 0 TSRMLS_CC);
            p = zend_memrchr(dt, '-', strlen(dt));
            if (!p || *(p + 5) != ' ')
            {
                efree(dt);
                efree(cookie);
                efree(encoded_value);
                php_error_docref(NULL TSRMLS_CC, E_WARNING, "Expiry date cannot have a year greater than 9999");
                RETURN_FALSE;
            }
            strlcat(cookie, dt, len + 100);
            efree(dt);
        }
    }
    if (encoded_value)
    {
        efree(encoded_value);
    }
    if (path && path_len > 0)
    {
        strlcat(cookie, "; path=", len + 100);
        strlcat(cookie, path, len + 100);
    }
    if (domain && domain_len > 0)
    {
        strlcat(cookie, "; domain=", len + 100);
        strlcat(cookie, domain, len + 100);
    }
    if (secure)
    {
        strlcat(cookie, "; secure", len + 100);
    }
    if (httponly)
    {
        strlcat(cookie, "; httponly", len + 100);
    }
    swString_append_ptr(client->response.cookie, cookie, strlen(cookie));
    swString_append_ptr(client->response.cookie, ZEND_STRL("\r\n"));
    efree(cookie);
}

PHP_METHOD(swoole_http_response, rawcookie)
{
    char *name, *value = NULL, *path = NULL, *domain = NULL;
    long expires = 0;
    int encode = 0;
    zend_bool secure = 0, httponly = 0;
    int name_len, value_len = 0, path_len = 0, domain_len = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|slssbb", &name, &name_len, &value, &value_len, &expires,
                &path, &path_len, &domain, &domain_len, &secure, &httponly) == FAILURE)
    {
        return;
    }

    zval *zfd = zend_read_property(swoole_http_response_class_entry_ptr, getThis(), ZEND_STRL("fd"), 0 TSRMLS_CC);
    if (ZVAL_IS_NULL(zfd))
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client not exists.");
        RETURN_FALSE;
    }

    http_client *client = swArray_fetch(http_client_array, Z_LVAL_P(zfd));
    if (!client)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client[#%d] not exists.", (int) Z_LVAL_P(zfd));
        RETURN_FALSE;
    }

    if (client->end)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Response is end.");
        RETURN_FALSE;
    }

    char *cookie, *encoded_value = NULL;
    int len = sizeof("Set-Cookie: ");
    char *dt;

    if (name && strpbrk(name, "=,; \t\r\n\013\014") != NULL)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cookie names cannot contain any of the following '=,; \\t\\r\\n\\013\\014'");
        RETURN_FALSE;
    }

    if (!client->response.cookie)
    {
        client->response.cookie = swString_new(1024);
    }

    len += name_len;
    if (encode && value)
    {
        int encoded_value_len;
        encoded_value = php_url_encode(value, value_len, &encoded_value_len);
        len += encoded_value_len;
    }
    else if (value)
    {
        encoded_value = estrdup(value);
        len += value_len;
    }
    if (path)
    {
        len += path_len;
    }
    if (domain)
    {
        len += domain_len;
    }

    cookie = emalloc(len + 100);

    if (value && value_len == 0)
    {
        dt = php_format_date("D, d-M-Y H:i:s T", sizeof("D, d-M-Y H:i:s T") - 1, 1, 0 TSRMLS_CC);
        snprintf(cookie, len + 100, "Set-Cookie: %s=deleted; expires=%s", name, dt);
        efree(dt);
    }
    else
    {
        snprintf(cookie, len + 100, "Set-Cookie: %s=%s", name, value ? encoded_value : "");
        if (expires > 0)
        {
            const char *p;
            strlcat(cookie, "; expires=", len + 100);
            dt = php_format_date("D, d-M-Y H:i:s T", sizeof("D, d-M-Y H:i:s T") - 1, expires, 0 TSRMLS_CC);
            p = zend_memrchr(dt, '-', strlen(dt));
            if (!p || *(p + 5) != ' ')
            {
                efree(dt);
                efree(cookie);
                efree(encoded_value);
                php_error_docref(NULL TSRMLS_CC, E_WARNING, "Expiry date cannot have a year greater than 9999");
                RETURN_FALSE;
            }
            strlcat(cookie, dt, len + 100);
            efree(dt);
        }
    }
    if (encoded_value)
    {
        efree(encoded_value);
    }
    if (path && path_len > 0)
    {
        strlcat(cookie, "; path=", len + 100);
        strlcat(cookie, path, len + 100);
    }
    if (domain && domain_len > 0)
    {
        strlcat(cookie, "; domain=", len + 100);
        strlcat(cookie, domain, len + 100);
    }
    if (secure)
    {
        strlcat(cookie, "; secure", len + 100);
    }
    if (httponly)
    {
        strlcat(cookie, "; httponly", len + 100);
    }
    swString_append_ptr(client->response.cookie, cookie, strlen(cookie));
    swString_append_ptr(client->response.cookie, ZEND_STRL("\r\n"));
    efree(cookie);
}

PHP_METHOD(swoole_http_response, status)
{
    long http_status;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &http_status) == FAILURE)
    {
        return;
    }

    zval *zfd = zend_read_property(swoole_http_response_class_entry_ptr, getThis(), ZEND_STRL("fd"), 0 TSRMLS_CC);

    http_client *client = swArray_fetch(http_client_array, Z_LVAL_P(zfd));
    if (!client)
    {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "http client[#%d] not exists.", (int) Z_LVAL_P(zfd));
        RETURN_FALSE;
    }

    client->response.status = http_status;
}

PHP_METHOD(swoole_http_response, header)
{
    char *k, *v;
    int klen, vlen;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &k, &klen, &v, &vlen) == FAILURE)
    {
        return;
    }
    zval *header = zend_read_property(swoole_http_request_class_entry_ptr, getThis(), ZEND_STRL("header"), 1 TSRMLS_CC);
    if (!header || ZVAL_IS_NULL(header))
    {
        MAKE_STD_ZVAL(header);
        array_init(header);
        zend_update_property(swoole_http_request_class_entry_ptr, getThis(), ZEND_STRL("header"), header TSRMLS_CC);
    }
    add_assoc_stringl_ex(header, k, klen + 1, v, vlen, 1);
}


