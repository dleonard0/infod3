#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "list.h"

struct el {
	LINK(struct el);
	unsigned char i;
};

#define BAD_INIT .next=(void *)1, .prevp=(void *)1

static struct el _e1 = { BAD_INIT, .i=1 };
static struct el _e2 = { BAD_INIT, .i=2 };
static struct el _e3 = { BAD_INIT, .i=3 };
#define e1 (&_e1)
#define e2 (&_e2)
#define e3 (&_e3)

__attribute__((sentinel))
static void
assert_same(const char *file, int line, const char *expr,
	    struct el *head, ...)
{
	struct el *exp;
	unsigned int pos;
	va_list ap;

	va_start(ap, head);
	for (pos = 0, exp = head;; pos++, exp = NEXT(exp)) {
		struct el *act = va_arg(ap, struct el *);
		if (act != exp) {
			fprintf(stderr, "%s:%d: failed %s\n"
					"\tposition: %u\n"
					"\texpected: %p [%d]\n"
					"\tactual:   %p [%d]\n",
				    file, line, expr,
				    pos,
				    exp, exp ? exp->i : -1,
				    act, act ? act->i : -1);
			abort();
		}
		if (!exp)
			break;
	}
}
#define ASSERT_SAME(head, ...) \
	assert_same(__FILE__, __LINE__, \
		"ASSERT_SAME(" #head ", {" #__VA_ARGS__ "})", \
		head, __VA_ARGS__)

int
main()
{
	struct el *head;
	struct el **tail;

	head = NULL;
	ASSERT_SAME(head, NULL);

	INSERT(e1, &head);
	ASSERT_SAME(head, e1, NULL);
	INSERT(e2, &head);
	ASSERT_SAME(head, e2, e1, NULL);
	REMOVE(e1);
	ASSERT_SAME(head, e2, NULL);
	REMOVE(e2);
	ASSERT_SAME(head, NULL);
	assert(!head);

	INSERT(e1, &head);
	ASSERT_SAME(head, e1, NULL);
	INSERT(e2, &head);
	ASSERT_SAME(head, e2, e1, NULL);
	REMOVE(e2);
	ASSERT_SAME(head, e1, NULL);
	REMOVE(e1);
	ASSERT_SAME(head, NULL);

	INSERT(e1, &head);
	ASSERT_SAME(head, e1, NULL);
	INSERT(e2, &head);
	ASSERT_SAME(head, e2, e1, NULL);
	INSERT(e3, &head);
	ASSERT_SAME(head, e3, e2, e1, NULL);
	REMOVE(e2);
	ASSERT_SAME(head, e3, e1, NULL);
	REMOVE(e3);
	ASSERT_SAME(head, e1, NULL);
	REMOVE(e1);
	ASSERT_SAME(head, NULL);
	assert(!head);

	/* append is okay as long as we never append after removing */
	head = NULL;
	tail = &head;
	APPEND(e1, &tail);
	assert(head == e1);
	assert(!e1->next);
	assert(e1->prevp == &head);
	assert(tail == &e1->next);
	ASSERT_SAME(head, e1, NULL);
	APPEND(e2, &tail);
	ASSERT_SAME(head, e1, e2, NULL);
	APPEND(e3, &tail);
	ASSERT_SAME(head, e1, e2, e3, NULL);
}
