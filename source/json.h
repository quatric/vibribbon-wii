/* Small DOM-style JSON parser. Strings are decoded in place, so the input
 * buffer must stay alive (and is modified) for as long as the tree is used. */
#ifndef JSON_H
#define JSON_H

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jnode {
	jtype_t type;
	char *key;               /* member name when inside an object */
	char *str;               /* J_STR */
	double num;              /* J_NUM, J_BOOL */
	struct jnode *child;     /* first element/member */
	struct jnode *next;
} jnode;

jnode *json_parse(char *text);
void json_free(jnode *n);

jnode *json_get(const jnode *obj, const char *key);
jnode *json_at(const jnode *arr, int index);
int json_len(const jnode *arr);
const char *json_str(const jnode *obj, const char *key);   /* "" if missing */
double json_num(const jnode *obj, const char *key, double def);

#endif
