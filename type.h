#ifndef TYPE_H
#define TYPE_H

#include "cc89.h"

struct type* new_type(enum type_kind kind, int size, int align);
struct type* basic_type(enum type_kind kind, int is_unsigned);
struct type* pointer_to(struct type* base);
struct type* array_of(struct type* base, int len);
struct type* func_type(struct type* ret);
struct type* promote(struct type* t);
struct type* usual_arith(struct type* a, struct type* b);

#endif
