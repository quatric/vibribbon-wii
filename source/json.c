#include <stdlib.h>
#include <string.h>
#include "json.h"

#define MAX_DEPTH 64

typedef struct {
	char *p;
	int depth;
} parser_t;

static jnode *parse_value(parser_t *ps);

static void skip_ws(parser_t *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
		ps->p++;
}

static jnode *new_node(jtype_t t)
{
	jnode *n = calloc(1, sizeof(jnode));
	if (n)
		n->type = t;
	return n;
}

static int hex4(const char *s, unsigned *out)
{
	unsigned v = 0;
	int i;
	for (i = 0; i < 4; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0';
		else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
		else return -1;
	}
	*out = v;
	return 0;
}

static char *put_utf8(char *o, unsigned cp)
{
	if (cp < 0x80) {
		*o++ = cp;
	} else if (cp < 0x800) {
		*o++ = 0xC0 | (cp >> 6);
		*o++ = 0x80 | (cp & 0x3F);
	} else if (cp < 0x10000) {
		*o++ = 0xE0 | (cp >> 12);
		*o++ = 0x80 | ((cp >> 6) & 0x3F);
		*o++ = 0x80 | (cp & 0x3F);
	} else {
		*o++ = 0xF0 | (cp >> 18);
		*o++ = 0x80 | ((cp >> 12) & 0x3F);
		*o++ = 0x80 | ((cp >> 6) & 0x3F);
		*o++ = 0x80 | (cp & 0x3F);
	}
	return o;
}

/* Decodes the string starting after the opening quote, in place. */
static char *parse_string(parser_t *ps)
{
	char *start = ps->p, *o = ps->p;

	for (;;) {
		char c = *ps->p++;
		if (c == 0)
			return NULL;
		if (c == '"')
			break;
		if (c != '\\') {
			*o++ = c;
			continue;
		}
		c = *ps->p++;
		switch (c) {
		case '"': case '\\': case '/': *o++ = c; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'n': *o++ = '\n'; break;
		case 'r': *o++ = '\r'; break;
		case 't': *o++ = '\t'; break;
		case 'u': {
			unsigned cp, lo;
			if (hex4(ps->p, &cp)) return NULL;
			ps->p += 4;
			if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u' &&
			    !hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				ps->p += 6;
			}
			o = put_utf8(o, cp);
			break;
		}
		default:
			return NULL;
		}
	}
	*o = 0;
	return start;
}

static jnode *parse_container(parser_t *ps, int is_obj)
{
	jnode *n = new_node(is_obj ? J_OBJ : J_ARR), *last = NULL;
	char close = is_obj ? '}' : ']';

	if (!n || ++ps->depth > MAX_DEPTH)
		goto fail;
	skip_ws(ps);
	if (*ps->p == close) {
		ps->p++;
		ps->depth--;
		return n;
	}
	for (;;) {
		char *key = NULL;
		jnode *v;

		skip_ws(ps);
		if (is_obj) {
			if (*ps->p++ != '"' || !(key = parse_string(ps)))
				goto fail;
			skip_ws(ps);
			if (*ps->p++ != ':')
				goto fail;
		}
		v = parse_value(ps);
		if (!v)
			goto fail;
		v->key = key;
		if (last) last->next = v;
		else n->child = v;
		last = v;

		skip_ws(ps);
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p++ == close)
			break;
		goto fail;
	}
	ps->depth--;
	return n;
fail:
	json_free(n);
	return NULL;
}

static jnode *parse_value(parser_t *ps)
{
	jnode *n;

	skip_ws(ps);
	switch (*ps->p) {
	case '{':
		ps->p++;
		return parse_container(ps, 1);
	case '[':
		ps->p++;
		return parse_container(ps, 0);
	case '"':
		ps->p++;
		n = new_node(J_STR);
		if (n && !(n->str = parse_string(ps))) {
			free(n);
			return NULL;
		}
		return n;
	case 't':
		if (strncmp(ps->p, "true", 4)) return NULL;
		ps->p += 4;
		n = new_node(J_BOOL);
		if (n) n->num = 1;
		return n;
	case 'f':
		if (strncmp(ps->p, "false", 5)) return NULL;
		ps->p += 5;
		return new_node(J_BOOL);
	case 'n':
		if (strncmp(ps->p, "null", 4)) return NULL;
		ps->p += 4;
		return new_node(J_NULL);
	default: {
		char *end;
		double d = strtod(ps->p, &end);
		if (end == ps->p) return NULL;
		ps->p = end;
		n = new_node(J_NUM);
		if (n) n->num = d;
		return n;
	}
	}
}

jnode *json_parse(char *text)
{
	parser_t ps = { text, 0 };
	jnode *root = parse_value(&ps);
	return root;
}

void json_free(jnode *n)
{
	while (n) {
		jnode *next = n->next;
		json_free(n->child);
		free(n);
		n = next;
	}
}

jnode *json_get(const jnode *obj, const char *key)
{
	jnode *c;
	if (!obj || obj->type != J_OBJ) return NULL;
	for (c = obj->child; c; c = c->next)
		if (c->key && !strcmp(c->key, key))
			return c;
	return NULL;
}

jnode *json_at(const jnode *arr, int index)
{
	jnode *c;
	if (!arr || arr->type != J_ARR) return NULL;
	for (c = arr->child; c && index > 0; c = c->next)
		index--;
	return c;
}

int json_len(const jnode *arr)
{
	int n = 0;
	jnode *c;
	if (!arr || arr->type != J_ARR) return 0;
	for (c = arr->child; c; c = c->next)
		n++;
	return n;
}

const char *json_str(const jnode *obj, const char *key)
{
	jnode *n = json_get(obj, key);
	return (n && n->type == J_STR) ? n->str : "";
}

double json_num(const jnode *obj, const char *key, double def)
{
	jnode *n = json_get(obj, key);
	return (n && n->type == J_NUM) ? n->num : def;
}
