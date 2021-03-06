/*
 * Copyright (c) 2017 - 2018, CUJO LLC.
 * Copyright (c) 2011 - 2016, Micro Systems Marc Balmer, CH-5073 Gipf-Oberfrick
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Micro Systems Marc Balmer nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* JSON interface for Lua */

/*
 * This code has been derived from the public domain LuaJSON Library 1.1
 * written by Nathaniel Musgrove (proton.zero@gmail.com), for the original
 * code see http://luaforge.net/projects/luajsonlib/
 */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#ifndef _KERNEL
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#endif

#define JSON_NULL_METATABLE 	"JSON null object methods"

static void decode_value(lua_State *, char **, int);
static void decode_string(lua_State *, char **);
static void encode(lua_State *, luaL_Buffer *, int);

static unsigned int
digit2int(lua_State *L, const unsigned char digit)
{
	unsigned int val;

	if (digit >= '0' && digit <= '9')
		val = digit - '0';
	else if (digit >= 'a' || digit <= 'f')
		val = digit - 'a' + 10;
	else if (digit >= 'A' || digit <= 'F')
		val = digit - 'A' + 10;
	else
		luaL_error(L, "Invalid hex digit");
	return val;
}

static unsigned int
fourhex2int(lua_State *L, const unsigned char *code)
{
	unsigned int utf = 0;

	utf += digit2int(L, code[0]) * 4096;
	utf += digit2int(L, code[1]) * 256;
	utf += digit2int(L, code[2]) * 16;
	utf += digit2int(L, code[3]);
	return utf;
}

static const char *
code2utf8(lua_State *L, const unsigned char *code, char buf[4])
{
	unsigned int utf = 0;

	utf = fourhex2int(L, code);
	if (utf < 128) {
		buf[0] = utf & 0x7F;
		buf[1] = buf[2] = buf[3] = 0;
	} else if (utf < 2048) {
		buf[0] = ((utf >> 6) & 0x1F) | 0xC0;
		buf[1] = (utf & 0x3F) | 0x80;
		buf[2] = buf[3] = 0;
	} else if (utf < 65536) {
		buf[0] = ((utf >> 12) & 0x0F) | 0xE0;
		buf[1] = ((utf >> 6) & 0x3F) | 0x80;
		buf[2] = (utf & 0x3F) | 0x80;
		buf[3] = 0;
	} else {
		buf[0] = ((utf >> 18) & 0x07) | 0xF0;
		buf[1] = ((utf >> 12) & 0x3F) | 0x80;
		buf[2] = ((utf >> 6) & 0x3F) | 0x80;
		buf[3] = (utf & 0x3F) | 0x80;
	}
	return buf;
}

static void
skip_ws(char **s)
{
	while (isspace(**s))
		(*s)++;
}

static void
decode_array(lua_State *L, char **s, int null)
{
	int i = 1;

	luaL_checkstack(L, 2, "Out of stack space");
	(*s)++;
	lua_newtable(L);
	for (i = 1; 1; i++) {
		skip_ws(s);
		lua_pushinteger(L, i);
		decode_value(L, s, null);
		lua_settable(L, -3);
		skip_ws(s);
		if (**s == ',') {
			(*s)++;
			skip_ws(s);
		} else
			break;
	}
	skip_ws(s);
	if (**s == ']')
		(*s)++;
	else
		luaL_error(L, "array does not end with ']'");
}

static void
decode_object(lua_State *L, char **s, int null)
{
	luaL_checkstack(L, 1, "Out of stack space");

	(*s)++;
	lua_newtable(L);
	skip_ws(s);

	if (**s != '}') {
		while (1) {
			skip_ws(s);
			decode_string(L, s);
			skip_ws(s);
			if (**s != ':')
				luaL_error(L, "object lacks separator ':'");
			(*s)++;
			skip_ws(s);
			decode_value(L, s, null);
			lua_settable(L, -3);
			skip_ws(s);
			if (**s == ',') {
				(*s)++;
				skip_ws(s);
			} else
				break;
		}
		skip_ws(s);
	}
	if (**s == '}')
		(*s)++;
	else
		luaL_error(L, "objects does not end with '}'");
}

static void
decode_string(lua_State *L, char **s)
{
	size_t len;
	char *beginning, *end, *nextEscape = NULL;
	char utfbuf[4] = "";
	luaL_Buffer b;

	luaL_checkstack(L, 1, "Out of stack space");
	(*s)++;
	beginning = *s;
	for (end = NULL; **s != '\0' && end == NULL; (*s)++) {
		if (**s == '"' && (*((*s) - 1) != '\\'))
			end = *s;
	}
	if (end == NULL)
		return;
	*s = beginning;
	luaL_buffinit(L, &b);
	while (*s != end) {
		nextEscape = strchr(*s, '\\');
		if (nextEscape > end)
			nextEscape = NULL;
		if (nextEscape == *s) {
			switch (*((*s) + 1)) {
			case '"':
				luaL_addchar(&b, '"');
				(*s) += 2;
				break;
			case '\\':
				luaL_addchar(&b, '\\');
				(*s) += 2;
				break;
			case '/':
				luaL_addchar(&b, '/');
				(*s) += 2;
				break;
			case 'b':
				luaL_addchar(&b, '\b');
				(*s) += 2;
				break;
			case 'f':
				luaL_addchar(&b, '\f');
				(*s) += 2;
				break;
			case 'n':
				luaL_addchar(&b, '\n');
				(*s) += 2;
				break;
			case 'r':
				luaL_addchar(&b, '\r');
				(*s) += 2;
				break;
			case 't':
				luaL_addchar(&b, '\t');
				(*s) += 2;
				break;
			case 'u':
				code2utf8(L, (unsigned char *)(*s) + 2, utfbuf);
				luaL_addstring(&b, utfbuf);
				(*s) += 6;
				break;
			default:
				luaL_error(L, "invalid escape character");
				break;
			}
		} else if (nextEscape != NULL) {
			len = nextEscape - *s;
			luaL_addlstring(&b, *s, len);
			(*s) += len;
		} else {
			len = end - *s;
			luaL_addlstring(&b, *s, len);
			(*s) += len;
		}
	}
	luaL_pushresult(&b);
	(*s)++;
}

static void
decode_value(lua_State *L, char **s, int null)
{
	skip_ws(s);

	luaL_checkstack(L, 1, "Out of stack space");
	if (!strncmp(*s, "false", 5)) {
		lua_pushboolean(L, 0);
		*s += 5;
	} else if (!strncmp(*s, "true", 4)) {
		lua_pushboolean(L, 1);
		*s += 4;
	} else if (!strncmp(*s, "null", 4)) {
		switch (null) {
		case 0:
			lua_pushstring(L, "");
			break;
		case 1:
			lua_newtable(L);
			luaL_setmetatable(L, JSON_NULL_METATABLE);
			break;
		case 2:
			lua_pushnil(L);
			break;
		}
		*s += 4;
	} else if (isdigit(**s) || **s == '+' || **s == '-') {
		char *p = *s;
#ifndef _KERNEL
		int isfloat = 0;

		/* advance pointer past the number */
		while (isdigit(**s) || **s == '+' || **s == '-'
		    || **s == 'e' || **s == 'E' || **s == '.') {
		    	if (**s == 'e' || **s == 'E' || **s == '.')
		    		isfloat = 1;
#else
		while (isdigit(**s)) {
#endif
			(*s)++;
		}
#ifndef _KERNEL
		if (isfloat)
			lua_pushnumber(L, strtod(p, NULL));
		else
#endif
			lua_pushinteger(L, strtoll(p, NULL, 10));
	} else {
		switch (**s) {
		case '[':
			decode_array(L, s, null);
			break;
		case '{':
			decode_object(L, s, null);
			break;
		case '"':
			decode_string(L, s);
			break;
		case ']':	/* ignore end of empty array */
			lua_pushnil(L);
			break;
		default:
			luaL_error(L, "syntax error");
			break;
		}
	}
}

static int
json_decode(lua_State *L)
{
	char *s;
	int null;
	const char *const options[] = {
		"empty-string",
		"json-null",
		"nil",
		NULL
	};

	s = (char *)luaL_checkstring(L, 1);

	null = luaL_checkoption(L, 2, "json-null", options);

	decode_value(L, &s, null);
	return 1;
}

/* encode JSON */

/* encode_string assumes an UTF-8 string */
static void
encode_string(lua_State *L, luaL_Buffer *b, unsigned char *s)
{
	char hexbuf[6];

	luaL_addchar(b, '"');
	for (; *s; s++) {
		switch (*s) {
		case '\\':
			luaL_addstring(b, "\\\\");
			break;
		case '"':
			luaL_addstring(b, "\\\"");
			break;
		case '\b':
			luaL_addstring(b, "\\b");
			break;
		case '\f':
			luaL_addstring(b, "\\f");
			break;
		case '\n':
			luaL_addstring(b, "\\n");
			break;
		case '\r':
			luaL_addstring(b, "\\r");
			break;
		case '\t':
			luaL_addstring(b, "\\t");
			break;
		default:
		/* Convert UTF-8 to unicode
		 * 00000000 - 0000007F: 0xxxxxxx
		 * 00000080 - 000007FF: 110xxxxx 10xxxxxx
		 * 00000800 - 0000FFFF: 1110xxxx 10xxxxxx 10xxxxxx
		 * 00010000 - 001FFFFF: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
		 */
			if ((*s & 0x80) == 0)
				luaL_addchar(b, *s);
			else if (((*s >> 5) & 0x07) == 0x06) {
				luaL_addstring(b, "\\u");
				snprintf(hexbuf, sizeof hexbuf, "%04x",
				    ((*s & 0x1f) << 6) | (*(s + 1) & 0x3f));
				luaL_addstring(b, hexbuf);
				s++;
			} else if (((*s >> 4) & 0x0f) == 0x0e) {
				luaL_addstring(b, "\\u");
				snprintf(hexbuf, sizeof hexbuf, "%04x",
				    ((*s & 0x0f) << 12) |
				    ((*(s + 1) & 0x3f) << 6) |
				    (*(s + 2) & 0x3f));
				luaL_addstring(b, hexbuf);
				s += 2;
			} else if (((*s >> 3) & 0x1f) == 0x1e) {
				luaL_addstring(b, "\\u");
				snprintf(hexbuf, sizeof hexbuf, "%04x",
				    ((*s & 0x07) << 18) |
				    ((*(s + 1) & 0x3f) << 12) |
				    ((*(s + 2) & 0x3f) << 6) |
				    (*(s + 3) & 0x3f));
				luaL_addstring(b, hexbuf);
				s += 3;
			}
			break;
		}
	}
	luaL_addchar(b, '"');
}

static void
encode(lua_State *L, luaL_Buffer *b, int arg)
{
	int n;

	switch (lua_type(L, arg)) {
	case LUA_TBOOLEAN:
		luaL_addstring(b, lua_toboolean(L, arg) ? "true" : "false");
		lua_remove(L, arg);
		break;
	case LUA_TNUMBER:
		lua_pushvalue(L, arg);
		luaL_addvalue(b);
		lua_remove(L, arg);
		break;
	case LUA_TSTRING:
		encode_string(L, b, (unsigned char *)lua_tostring(L, arg));
		lua_remove(L, arg);
		break;
	case LUA_TTABLE:
		/* check if this is the null value */
		luaL_checkstack(L, 2, "out of stack space");
		if (lua_getmetatable(L, arg)) {
			luaL_getmetatable(L, JSON_NULL_METATABLE);
			if (lua_compare(L, -2, -1, LUA_OPEQ)) {
				lua_pop(L, 2);
				luaL_addstring(b, "null");
				lua_pop(L, 1);
				break;
			}
			lua_pop(L, 2);
		}
		/* if there are t[1] .. t[n], output them as array */
		for (n = 0; ; n++) {
			lua_geti(L, arg, n + 1);
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				break;
			}
			lua_insert(L, arg + 1);
			luaL_addchar(b, n ? ',' : '[');
			encode(L, b, arg + 1);
		}
		if (n) {
			luaL_addchar(b, ']');
			lua_remove(L, arg);
			break;
		}

		/* output non-numerical indices as object */
		lua_pushnil(L);
		n = 0;
		while (lua_next(L, arg) != 0) {
			if (lua_type(L, -2) == LUA_TNUMBER) {
				lua_pop(L, 1);
				continue;
			}
			lua_insert(L, arg + 1); /* value */
			lua_insert(L, arg + 1); /* key */
			luaL_addstring(b, n ? ",\"" : "{\"");
			luaL_addstring(b, lua_tostring(L, arg + 1));
			luaL_addstring(b, "\":");
			encode(L, b, arg + 2);
			lua_pushvalue(L, arg + 1); /* key */
			lua_remove(L, arg + 1);
			n++;
		}
		if (n)
			luaL_addchar(b, '}');
		else
			luaL_addstring(b, "[]");
		lua_remove(L, arg);
		break;
	case LUA_TNIL:
		luaL_addstring(b, "null");
		lua_remove(L, arg);
		break;
	default:
		luaL_error(L, "Lua type %s is incompatible with JSON",
		    luaL_typename(L, arg));
		lua_remove(L, arg);
	}
}

static int
json_encode(lua_State *L)
{
	luaL_Buffer b;

	luaL_checkany(L, 1);
	luaL_buffinit(L, &b);
	encode(L, &b, 1);
	luaL_pushresult(&b);
	return 1;
}

static int
json_isnull(lua_State *L)
{
	if (lua_getmetatable(L, -1)) {
		luaL_getmetatable(L, JSON_NULL_METATABLE);
		if (lua_compare(L, -2, -1, LUA_OPEQ)) {
			lua_pop(L, 2);
			lua_pushboolean(L, 1);
			goto done;
		}
		lua_pop(L, 2);
	}
	lua_pushboolean(L, 0);
done:
	return 1;
}

static void
json_set_info(lua_State *L)
{
	lua_pushliteral(L, "_COPYRIGHT");
	lua_pushliteral(L, "Copyright (C) 2011 - 2016 "
	    "micro systems marc balmer");
	lua_settable(L, -3);
	lua_pushliteral(L, "_DESCRIPTION");
	lua_pushliteral(L, "JSON encoder/decoder for Lua");
	lua_settable(L, -3);
	lua_pushliteral(L, "_VERSION");
	lua_pushliteral(L, "json 1.2.10");
	lua_settable(L, -3);
}

static int
json_null(lua_State *L)
{
	lua_pushstring(L, "null");
	return 1;
}

int
luaopen_json(lua_State* L)
{
	static const struct luaL_Reg methods[] = {
		{ "decode",	json_decode },
		{ "encode",	json_encode },
		{ "isnull",	json_isnull },
		{ NULL,		NULL }
	};
	static const struct luaL_Reg null_methods[] = {
		{ "__tostring",	json_null },
		{ "__call",	json_null },
		{ NULL,		NULL }
	};

	luaL_newlib(L, methods);
	json_set_info(L);

	lua_newtable(L);
	/* The null metatable */
	if (luaL_newmetatable(L, JSON_NULL_METATABLE)) {
		luaL_setfuncs(L, null_methods, 0);
		lua_pushliteral(L, "__metatable");
		lua_pushliteral(L, "must not access this metatable");
		lua_settable(L, -3);

	}
	lua_setmetatable(L, -2);

	lua_setfield(L, -2, "null");
	return 1;
}
#ifdef _KERNEL
#include <linux/module.h>

int
__init modinit(void)
{
	return 0;
}

void
__exit modexit(void)
{
}

module_init(modinit);
module_exit(modexit);

EXPORT_SYMBOL(luaopen_json);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("CUJO LLC <opensource@cujo.com>");
#endif
