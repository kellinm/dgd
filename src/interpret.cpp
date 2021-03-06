/*
 * This file is part of DGD, https://github.com/dworkin/dgd
 * Copyright (C) 1993-2010 Dworkin B.V.
 * Copyright (C) 2010-2017 DGD Authors (see the commit log for details)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

# include "dgd.h"
# include "str.h"
# include "array.h"
# include "object.h"
# include "xfloat.h"
# include "interpret.h"
# include "data.h"
# include "control.h"
# include "table.h"

# ifdef DEBUG
# undef EXTRA_STACK
# define EXTRA_STACK  0
# endif


static Value stack[MIN_STACK];	/* initial stack */
static Frame topframe;		/* top frame */
static rlinfo rlim;		/* top rlimits info */
Frame *cframe;			/* current frame */
static char *creator;		/* creator function name */
static unsigned int clen;	/* creator function name length */
static bool stricttc;		/* strict typechecking */
static char ihash[INHASHSZ];	/* instanceof hashtable */

int nil_type;			/* type of nil value */
Value zero_int = { T_INT, TRUE };
Value zero_float = { T_FLOAT, TRUE };
Value nil_value = { T_NIL, TRUE };

/*
 * NAME:	interpret->init()
 * DESCRIPTION:	initialize the interpreter
 */
void i_init(char *create, bool flag)
{
    topframe.oindex = OBJ_NONE;
    topframe.fp = topframe.sp = stack + MIN_STACK;
    topframe.stack = stack;
    rlim.maxdepth = 0;
    rlim.ticks = 0;
    rlim.nodepth = TRUE;
    rlim.noticks = TRUE;
    topframe.rlim = &rlim;
    topframe.level = 0;
    topframe.atomic = FALSE;
    cframe = &topframe;

    creator = create;
    clen = strlen(create);
    stricttc = flag;

    nil_value.type = nil_type = (stricttc) ? T_NIL : T_INT;
}

/*
 * NAME:	interpret->ref_value()
 * DESCRIPTION:	reference a value
 */
void i_ref_value(Value *v)
{
    switch (v->type) {
    case T_STRING:
	str_ref(v->u.string);
	break;

    case T_ARRAY:
    case T_MAPPING:
    case T_LWOBJECT:
	arr_ref(v->u.array);
	break;
    }
}

/*
 * NAME:	interpret->del_value()
 * DESCRIPTION:	dereference a value (not an lvalue)
 */
void i_del_value(Value *v)
{
    switch (v->type) {
    case T_STRING:
	str_del(v->u.string);
	break;

    case T_ARRAY:
    case T_MAPPING:
    case T_LWOBJECT:
	arr_del(v->u.array);
	break;
    }
}

/*
 * NAME:	interpret->copy()
 * DESCRIPTION:	copy values from one place to another
 */
void i_copy(Value *v, Value *w, unsigned int len)
{
    Value *o;

    for ( ; len != 0; --len) {
	switch (w->type) {
	case T_STRING:
	    str_ref(w->u.string);
	    break;

	case T_OBJECT:
	    if (DESTRUCTED(w)) {
		*v++ = nil_value;
		w++;
		continue;
	    }
	    break;

	case T_LWOBJECT:
	    o = d_get_elts(w->u.array);
	    if (o->type == T_OBJECT && DESTRUCTED(o)) {
		*v++ = nil_value;
		w++;
		continue;
	    }
	    /* fall through */
	case T_ARRAY:
	case T_MAPPING:
	    arr_ref(w->u.array);
	    break;
	}
	*v++ = *w++;
    }
}

/*
 * NAME:	interpret->grow_stack()
 * DESCRIPTION:	check if there is room on the stack for new values; if not,
 *		make space
 */
void i_grow_stack(Frame *f, int size)
{
    if (f->sp < f->stack + size + MIN_STACK) {
	int spsize;
	Value *v, *stk;
	intptr_t offset;

	/*
	 * extend the local stack
	 */
	spsize = f->fp - f->sp;
	size = ALGN(spsize + size + MIN_STACK, 8);
	stk = ALLOC(Value, size);
	offset = (intptr_t) (stk + size) - (intptr_t) f->fp;

	/* move stack values */
	v = stk + size;
	if (spsize != 0) {
	    memcpy(v - spsize, f->sp, spsize * sizeof(Value));
	}
	f->sp = v - spsize;

	/* replace old stack */
	if (f->sos) {
	    /* stack on stack: alloca'd */
	    AFREE(f->stack);
	    f->sos = FALSE;
	} else if (f->stack != stack) {
	    FREE(f->stack);
	}
	f->stack = stk;
	f->fp = stk + size;
    }
}

/*
 * NAME:	interpret->push_value()
 * DESCRIPTION:	push a value on the stack
 */
void i_push_value(Frame *f, Value *v)
{
    Value *o;

    *--f->sp = *v;
    switch (v->type) {
    case T_STRING:
	str_ref(v->u.string);
	break;

    case T_OBJECT:
	if (DESTRUCTED(v)) {
	    /*
	     * can't wipe out the original, since it may be a value from a
	     * mapping
	     */
	    *f->sp = nil_value;
	}
	break;

    case T_LWOBJECT:
	o = d_get_elts(v->u.array);
	if (o->type == T_OBJECT && DESTRUCTED(o)) {
	    /*
	     * can't wipe out the original, since it may be a value from a
	     * mapping
	     */
	    *f->sp = nil_value;
	    break;
	}
	/* fall through */
    case T_ARRAY:
    case T_MAPPING:
	arr_ref(v->u.array);
	break;
    }
}

/*
 * NAME:	interpret->pop()
 * DESCRIPTION:	pop a number of values (can be lvalues) from the stack
 */
void i_pop(Frame *f, int n)
{
    Value *v;

    for (v = f->sp; --n >= 0; v++) {
	switch (v->type) {
	case T_STRING:
	    str_del(v->u.string);
	    break;

	case T_ARRAY:
	case T_MAPPING:
	case T_LWOBJECT:
	    arr_del(v->u.array);
	    break;
	}
    }
    f->sp = v;
}

/*
 * NAME:	interpret->odest()
 * DESCRIPTION:	replace all occurrences of an object on the stack by nil
 */
void i_odest(Frame *prev, Object *obj)
{
    Frame *f;
    Uint count;
    Value *v;
    unsigned short n;

    count = obj->count;

    /* wipe out objects in stack frames */
    for (;;) {
	f = prev;
	for (v = f->sp; v < f->fp; v++) {
	    switch (v->type) {
	    case T_OBJECT:
		if (v->u.objcnt == count) {
		    *v = nil_value;
		}
		break;

	    case T_LWOBJECT:
		if (v->u.array->elts[0].type == T_OBJECT &&
		    v->u.array->elts[0].u.objcnt == count) {
		    arr_del(v->u.array);
		    *v = nil_value;
		}
		break;
	    }
	}

	prev = f->prev;
	if (prev == (Frame *) NULL) {
	    break;
	}
	if ((f->func->sclass & C_ATOMIC) && !prev->atomic) {
	    /*
	     * wipe out objects in arguments to atomic function call
	     */
	    for (n = f->nargs, v = prev->sp; n != 0; --n, v++) {
		switch (v->type) {
		case T_OBJECT:
		    if (v->u.objcnt == count) {
			*v = nil_value;
		    }
		    break;

		case T_LWOBJECT:
		    if (v->u.array->elts[0].type == T_OBJECT &&
			v->u.array->elts[0].u.objcnt == count) {
			arr_del(v->u.array);
			*v = nil_value;
		    }
		    break;
		}
	    }
	    break;
	}
    }
}

/*
 * NAME:	interpret->string()
 * DESCRIPTION:	push a string constant on the stack
 */
void i_string(Frame *f, int inherit, unsigned int index)
{
    PUSH_STRVAL(f, d_get_strconst(f->p_ctrl, inherit, index));
}

/*
 * NAME:	interpret->aggregate()
 * DESCRIPTION:	create an array on the stack
 */
void i_aggregate(Frame *f, unsigned int size)
{
    Array *a;

    if (size == 0) {
	a = arr_new(f->data, 0L);
    } else {
	Value *v, *elts;

	i_add_ticks(f, size);
	a = arr_new(f->data, (long) size);
	elts = a->elts + size;
	v = f->sp;
	do {
	    *--elts = *v++;
	} while (--size != 0);
	d_ref_imports(a);
	f->sp = v;
    }
    PUSH_ARRVAL(f, a);
}

/*
 * NAME:	interpret->map_aggregate()
 * DESCRIPTION:	create a mapping on the stack
 */
void i_map_aggregate(Frame *f, unsigned int size)
{
    Array *a;

    if (size == 0) {
	a = map_new(f->data, 0L);
    } else {
	Value *v, *elts;

	i_add_ticks(f, size);
	a = map_new(f->data, (long) size);
	elts = a->elts + size;
	v = f->sp;
	do {
	    *--elts = *v++;
	} while (--size != 0);
	f->sp = v;
	try {
	    ec_push((ec_ftn) NULL);
	    map_sort(a);
	    ec_pop();
	} catch (...) {
	    /* error in sorting, delete mapping and pass on error */
	    arr_ref(a);
	    arr_del(a);
	    error((char *) NULL);
	}
	d_ref_imports(a);
    }
    PUSH_MAPVAL(f, a);
}

/*
 * NAME:	interpret->spread1()
 * DESCRIPTION:	push the values in an array on the stack, return the number of
 *		extra arguments pushed
 */
int i_spread1(Frame *f, int n)
{
    Array *a;
    int i;
    Value *v;

    if (f->sp->type != T_ARRAY) {
	error("Spread of non-array");
    }
    a = f->sp->u.array;

    if (n < 0) {
	/* no lvalues */
	n = a->size;
	i_add_ticks(f, n);
	f->sp++;
	i_grow_stack(f, n);
	for (i = 0, v = d_get_elts(a); i < n; i++, v++) {
	    i_push_value(f, v);
	}
	arr_del(a);

	return n - 1;
    } else {
	/* including lvalues */
	if (n > a->size) {
	    n = a->size;
	}
	i_add_ticks(f, n);
	i_grow_stack(f, n);
	f->sp++;
	for (i = 0, v = d_get_elts(a); i < n; i++, v++) {
	    i_push_value(f, v);
	}
	--f->sp;
	PUT_ARRVAL_NOREF(f->sp, a);

	return n;
    }
}

/*
 * NAME:	interpret->global()
 * DESCRIPTION:	push a global value on the stack
 */
void i_global(Frame *f, int inherit, int index)
{
    i_add_ticks(f, 4);
    inherit = UCHAR(f->ctrl->imap[f->p_index + inherit]);
    inherit = f->ctrl->inherits[inherit].varoffset;
    if (f->lwobj == (Array *) NULL) {
	i_push_value(f, d_get_variable(f->data, inherit + index));
    } else {
	i_push_value(f, &f->lwobj->elts[2 + inherit + index]);
    }
}

/*
 * NAME:	interpret->operator()
 * DESCRIPTION:	index or indexed assignment
 */
static void i_operator(Frame *f, Array *lwobj, const char *op, int nargs,
		       Value *var, Value *idx, Value *val)
{
    i_push_value(f, idx);
    if (nargs > 1) {
	i_push_value(f, val);
    }
    if (!i_call(f, (Object *) NULL, lwobj, op, strlen(op), TRUE, nargs)) {
	error("Index on bad type");
    }

    *var = *f->sp++;
}

/*
 * NAME:	interpret->index2()
 * DESCRIPTION:	index a value
 */
void i_index2(Frame *f, Value *aval, Value *ival, Value *val, bool keep)
{
    int i;

    i_add_ticks(f, 2);
    switch (aval->type) {
    case T_STRING:
	if (ival->type != T_INT) {
	    error("Non-numeric string index");
	}
	i = UCHAR(aval->u.string->text[str_index(aval->u.string,
						 ival->u.number)]);
	if (!keep) {
	    str_del(aval->u.string);
	}
	PUT_INTVAL(val, i);
	return;

    case T_ARRAY:
	if (ival->type != T_INT) {
	    error("Non-numeric array index");
	}
	*val = d_get_elts(aval->u.array)[arr_index(aval->u.array,
						   ival->u.number)];
	break;

    case T_MAPPING:
	*val = *map_index(f->data, aval->u.array, ival, NULL, NULL);
	if (!keep) {
	    i_del_value(ival);
	}
	break;

    case T_LWOBJECT:
	i_operator(f, aval->u.array, "[]", 1, val, ival, (Value *) NULL);
	if (!keep) {
	    i_del_value(ival);
	    arr_del(aval->u.array);
	}
	return;

    default:
	error("Index on bad type");
    }

    switch (val->type) {
    case T_STRING:
	str_ref(val->u.string);
	break;

    case T_OBJECT:
	if (DESTRUCTED(val)) {
	    *val = nil_value;
	}
	break;

    case T_LWOBJECT:
	ival = d_get_elts(val->u.array);
	if (ival->type == T_OBJECT && DESTRUCTED(ival)) {
	    *val = nil_value;
	    break;
	}
	/* fall through */
    case T_ARRAY:
    case T_MAPPING:
	arr_ref(val->u.array);
	break;
    }

    if (!keep) {
	arr_del(aval->u.array);
    }
}

/*
 * NAME:	interpret->typename()
 * DESCRIPTION:	return the name of the argument type
 */
char *i_typename(char *buf, unsigned int type)
{
    static const char *name[] = TYPENAMES;

    if ((type & T_TYPE) == T_CLASS) {
	type = (type & T_REF) | T_OBJECT;
    }
    strcpy(buf, name[type & T_TYPE]);
    type &= T_REF;
    type >>= REFSHIFT;
    if (type > 0) {
	char *p;

	p = buf + strlen(buf);
	*p++ = ' ';
	do {
	    *p++ = '*';
	} while (--type > 0);
	*p = '\0';
    }
    return buf;
}

/*
 * NAME:	interpret->classname()
 * DESCRIPTION:	return the name of a class
 */
char *i_classname(Frame *f, Uint sclass)
{
    return d_get_strconst(f->p_ctrl, sclass >> 16, sclass & 0xffff)->text;
}

/*
 * NAME:	instanceof()
 * DESCRIPTION:	is an object an instance of the named program?
 */
static int instanceof(unsigned int oindex, char *prog, Uint hash)
{
    char *h;
    unsigned short i;
    dinherit *inh;
    Object *obj;
    Control *ctrl;

    /* first try hash table */
    obj = OBJR(oindex);
    if (!(obj->flags & O_MASTER)) {
	oindex = obj->u_master;
	obj = OBJR(oindex);
    }
    ctrl = o_control(obj);
    h = &ihash[((oindex << 2) ^ hash) % INHASHSZ];
    if (*h < ctrl->ninherits &&
	strcmp(OBJR(ctrl->inherits[UCHAR(*h)].oindex)->name, prog) == 0) {
	return (ctrl->inherits[UCHAR(*h)].priv) ? -1 : 1;	/* found it */
    }

    /* next, search for it the hard way */
    for (i = ctrl->ninherits, inh = ctrl->inherits + i; i != 0; ) {
	--i;
	--inh;
	if (strcmp(prog, OBJR(inh->oindex)->name) == 0) {
	    /* found it; update hashtable */
	    *h = i;
	    return (ctrl->inherits[i].priv) ? -1 : 1;
	}
    }
    return FALSE;
}

/*
 * NAME:	interpret->instanceof()
 * DESCRIPTION:	is an object an instance of the named program?
 */
int i_instanceof(Frame *f, unsigned int oindex, Uint sclass)
{
    return instanceof(oindex, i_classname(f, sclass), sclass);
}

/*
 * NAME:	interpret->instancestr()
 * DESCRIPTION:	is an object an instance of the named program?
 */
int i_instancestr(unsigned int oindex, char *prog)
{
    return instanceof(oindex, prog, Hashtab::hashstr(prog, OBJHASHSZ));
}

/*
 * NAME:	interpret->cast()
 * DESCRIPTION:	cast a value to a type
 */
void i_cast(Frame *f, Value *val, unsigned int type, Uint sclass)
{
    char tnbuf[TNBUFSIZE];
    Value *elts;

    if (type == T_CLASS) {
	if (val->type == T_OBJECT) {
	    if (!i_instanceof(f, val->oindex, sclass)) {
		error("Value is not of object type /%s", i_classname(f, sclass));
	    }
	    return;
	} else if (val->type == T_LWOBJECT) {
	    elts = d_get_elts(val->u.array);
	    if (elts->type == T_OBJECT) {
		if (!i_instanceof(f, elts->oindex, sclass)) {
		    error("Value is not of object type /%s",
			  i_classname(f, sclass));
		}
	    } else if (strcmp(o_builtin_name(elts->u.number),
			      i_classname(f, sclass)) != 0) {
		/*
		 * builtin types can only be cast to their own type
		 */
		error("Value is not of object type /%s", i_classname(f, sclass));
	    }
	    return;
	}
	type = T_OBJECT;
    }
    if (val->type != type && (val->type != T_LWOBJECT || type != T_OBJECT) &&
	(!VAL_NIL(val) || !T_POINTER(type))) {
	i_typename(tnbuf, type);
	if (strchr("aeiuoy", tnbuf[0]) != (char *) NULL) {
	    error("Value is not an %s", tnbuf);
	} else {
	    error("Value is not a %s", tnbuf);
	}
    }
}

/*
 * NAME:	interpret->store_local()
 * DESCRIPTION:	assign a value to a local variable
 */
static void i_store_local(Frame *f, int local, Value *val, Value *verify)
{
    Value *var;

    i_add_ticks(f, 1);
    var = (local < 0) ? f->fp + local : f->argp + local;
    if (verify == NULL ||
	(var->type == T_STRING && var->u.string == verify->u.string)) {
	d_assign_var(f->data, var, val);
    }
}

/*
 * NAME:	interpret->store_global()
 * DESCRIPTION:	assign a value to a global variable
 */
void i_store_global(Frame *f, int inherit, int index, Value *val, Value *verify)
{
    unsigned short offset;
    Value *var;

    i_add_ticks(f, 5);
    inherit = f->ctrl->imap[f->p_index + inherit];
    offset = f->ctrl->inherits[inherit].varoffset + index;
    if (f->lwobj == NULL) {
	var = d_get_variable(f->data, offset);
	if (verify == NULL ||
	    (var->type == T_STRING && var->u.string == verify->u.string)) {
	    d_assign_var(f->data, var, val);
	}
    } else {
	var = &f->lwobj->elts[2 + offset];
	if (verify == NULL ||
	    (var->type == T_STRING && var->u.string == verify->u.string)) {
	    d_assign_elt(f->data, f->lwobj, var, val);
	}
    }
}

/*
 * NAME:	interpret->store_index()
 * DESCRIPTION:	perform an indexed assignment
 */
bool i_store_index(Frame *f, Value *var, Value *aval, Value *ival, Value *val)
{
    ssizet i;
    String *str;
    Array *arr;

    i_add_ticks(f, 3);
    switch (aval->type) {
    case T_STRING:
	if (ival->type != T_INT) {
	    error("Non-numeric string index");
	}
	if (val->type != T_INT) {
	    error("Non-numeric value in indexed string assignment");
	}
	i = str_index(aval->u.string, ival->u.number);
	str = str_new(aval->u.string->text, aval->u.string->len);
	str->text[i] = val->u.number;
	PUT_STRVAL(var, str);
	return TRUE;

    case T_ARRAY:
	if (ival->type != T_INT) {
	    error("Non-numeric array index");
	}
	arr = aval->u.array;
	aval = &d_get_elts(arr)[arr_index(arr, ival->u.number)];
	if (var->type != T_STRING ||
	    (aval->type == T_STRING && var->u.string == aval->u.string)) {
	    d_assign_elt(f->data, arr, aval, val);
	}
	arr_del(arr);
	break;

    case T_MAPPING:
	arr = aval->u.array;
	if (var->type != T_STRING) {
	    var = NULL;
	}
	map_index(f->data, arr, ival, val, var);
	i_del_value(ival);
	arr_del(arr);
	break;

    case T_LWOBJECT:
	arr = aval->u.array;
	i_operator(f, arr, "[]=", 2, var, ival, val);
	i_del_value(var);
	i_del_value(ival);
	arr_del(arr);
	break;

    default:
	error("Index on bad type");
    }

    return FALSE;
}

/*
 * NAME:	interpret->stores()
 * DESCRIPTION:	perform a sequence of special stores
 */
static void i_stores(Frame *f, int skip, int assign)
{
    char *pc;
    unsigned short u, u2, instr;
    Uint sclass;
    Value val;

    pc = f->pc;
    instr = 0;

    /*
     * stores to skip
     */
    while (skip != 0) {
	instr = FETCH1U(pc);
	switch (instr & I_INSTR_MASK) {
	case I_CAST:
	case I_CAST | I_POP_BIT:
	    if (FETCH1U(pc) == T_CLASS) {
		pc += 3;
	    }
	    continue;

	case I_STORE_LOCAL:
	case I_STORE_LOCAL | I_POP_BIT:
	case I_STORE_GLOBAL:
	case I_STORE_GLOBAL | I_POP_BIT:
	    pc++;
	    break;

	case I_STORE_FAR_GLOBAL:
	case I_STORE_FAR_GLOBAL | I_POP_BIT:
	    pc += 2;
	    break;

	case I_STORE_INDEX:
	case I_STORE_INDEX | I_POP_BIT:
	    i_del_value(&f->sp[1]);
	    i_del_value(&f->sp[2]);
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_LOCAL_INDEX:
	case I_STORE_LOCAL_INDEX | I_POP_BIT:
	case I_STORE_GLOBAL_INDEX:
	case I_STORE_GLOBAL_INDEX | I_POP_BIT:
	    pc++;
	    i_del_value(&f->sp[1]);
	    i_del_value(&f->sp[2]);
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_FAR_GLOBAL_INDEX:
	case I_STORE_FAR_GLOBAL_INDEX | I_POP_BIT:
	    pc += 2;
	    i_del_value(&f->sp[1]);
	    i_del_value(&f->sp[2]);
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_INDEX_INDEX:
	case I_STORE_INDEX_INDEX | I_POP_BIT:
	    i_del_value(&f->sp[1]);
	    i_del_value(&f->sp[2]);
	    i_del_value(&f->sp[3]);
	    i_del_value(&f->sp[4]);
	    f->sp[4] = f->sp[0];
	    f->sp += 4;
	    break;

# ifdef DEBUG
	default:
	    fatal("invalid store");
# endif
	}
	--skip;
    }

    /*
     * stores to perform
     */
    sclass = 0;
    while (assign != 0) {
	instr = FETCH1U(pc);
	switch (instr & I_INSTR_MASK) {
	case I_CAST:
	case I_CAST | I_POP_BIT:
	    u = FETCH1U(pc);
	    if (u == T_CLASS) {
		FETCH3U(pc, sclass);
	    }
	    i_cast(f, &f->sp->u.array->elts[assign - 1], u, sclass);
	    continue;

	case I_STORE_LOCAL:
	case I_STORE_LOCAL | I_POP_BIT:
	    i_store_local(f, FETCH1S(pc), &f->sp->u.array->elts[assign - 1],
			  (Value *) NULL);
	    break;

	case I_STORE_GLOBAL:
	case I_STORE_GLOBAL | I_POP_BIT:
	    i_store_global(f, f->p_ctrl->ninherits - 1, FETCH1U(pc),
			   &f->sp->u.array->elts[assign - 1], (Value *) NULL);
	    break;

	case I_STORE_FAR_GLOBAL:
	case I_STORE_FAR_GLOBAL | I_POP_BIT:
	    u = FETCH1U(pc);
	    i_store_global(f, u, FETCH1U(pc),
			   &f->sp->u.array->elts[assign - 1], (Value *) NULL);
	    break;

	case I_STORE_INDEX:
	case I_STORE_INDEX | I_POP_BIT:
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1,
			      &f->sp->u.array->elts[assign - 1])) {
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_LOCAL_INDEX:
	case I_STORE_LOCAL_INDEX | I_POP_BIT:
	    u = FETCH1S(pc);
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1,
			      &f->sp->u.array->elts[assign - 1])) {
		i_store_local(f, (short) u, &val, &f->sp[2]);
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_GLOBAL_INDEX:
	case I_STORE_GLOBAL_INDEX | I_POP_BIT:
	    u = FETCH1U(pc);
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1,
			      &f->sp->u.array->elts[assign - 1])) {
		i_store_global(f, f->p_ctrl->ninherits - 1, u, &val, &f->sp[2]);
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_FAR_GLOBAL_INDEX:
	case I_STORE_FAR_GLOBAL_INDEX | I_POP_BIT:
	    u = FETCH1U(pc);
	    u2 = FETCH1U(pc);
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1,
			      &f->sp->u.array->elts[assign - 1])) {
		i_store_global(f, u, u2, &val, &f->sp[2]);
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_INDEX_INDEX:
	case I_STORE_INDEX_INDEX | I_POP_BIT:
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1,
			      &f->sp->u.array->elts[assign - 1])) {
		f->sp[1] = val;
		i_store_index(f, f->sp + 2, f->sp + 4, f->sp + 3, f->sp + 1);
		str_del(f->sp[1].u.string);
		str_del(f->sp[2].u.string);
	    } else {
		i_del_value(f->sp + 3);
		i_del_value(f->sp + 4);
	    }
	    f->sp[4] = f->sp[0];
	    f->sp += 4;
	    break;

# ifdef DEBUG
	default:
	    fatal("invalid store");
# endif
	}
	--assign;
    }

    if (instr & I_POP_BIT) {
	arr_del(f->sp->u.array);
	f->sp++;
    }

    f->pc = pc;
}

/*
 * NAME:	interpret->lvalues()
 * DESCRIPTION:	perform assignments for lvalue arguments
 */
void i_lvalues(Frame *f)
{
    char *pc;
    int n, offset, type;
    unsigned short nassign, nspread;
    Uint sclass;

    pc = f->pc;
# ifdef DEBUG
    if ((FETCH1U(pc) & I_INSTR_MASK) != I_STORES) {
	fatal("stores expected");
    }
# else
    pc++;
# endif
    n = FETCH1U(pc);
    f->pc = pc;

    if (n != 0) {
	nassign = f->sp->u.array->size;

	if ((FETCH1U(pc) & I_INSTR_MASK) == I_SPREAD) {
	    /*
	     * lvalue spread
	     */
	    sclass = 0;
	    offset = FETCH1U(pc);
	    type = FETCH1U(pc);
	    if (type == T_CLASS) {
		FETCH3U(pc, sclass);
	    }
	    f->pc = pc;

	    if (--n < nassign && f->sp[1].u.array->size > offset) {
		nspread = f->sp[1].u.array->size - offset;
		if (nspread >= nassign - n) {
		    nspread = nassign - n;
		    i_add_ticks(f, nspread * 3);
		    while (nspread != 0) {
			--nassign;
			if (type != 0) {
			    i_cast(f, &f->sp->u.array->elts[nassign], type,
				   sclass);
			}
			--nspread;
			d_assign_elt(f->data, f->sp[1].u.array,
				     &f->sp[1].u.array->elts[offset + nspread],
				     &f->sp->u.array->elts[nassign]);
		    }
		}
	    }

	    arr_del(f->sp[1].u.array);
	    f->sp[1] = f->sp[0];
	    f->sp++;
	}

	if (n < nassign) {
	    error("Missing lvalue");
	}
	i_stores(f, n - nassign, nassign);
    }
}

/*
 * NAME:	interpret->get_depth()
 * DESCRIPTION:	get the remaining stack depth (-1: infinite)
 */
Int i_get_depth(Frame *f)
{
    rlinfo *rlim;

    rlim = f->rlim;
    if (rlim->nodepth) {
	return -1;
    }
    return rlim->maxdepth - f->depth;
}

/*
 * NAME:	interpret->get_ticks()
 * DESCRIPTION:	get the remaining ticks (-1: infinite)
 */
Int i_get_ticks(Frame *f)
{
    rlinfo *rlim;

    rlim = f->rlim;
    if (rlim->noticks) {
	return -1;
    } else {
	return (rlim->ticks < 0) ? 0 : rlim->ticks << f->level;
    }
}

/*
 * NAME:	interpret->check_rlimits()
 * DESCRIPTION:	check if this rlimits call is valid
 */
static void i_check_rlimits(Frame *f)
{
    Object *obj;

    obj = OBJR(f->oindex);
    if (obj->count == 0) {
	error("Illegal use of rlimits");
    }
    --f->sp;
    f->sp[0] = f->sp[1];
    f->sp[1] = f->sp[2];
    if (f->lwobj == (Array *) NULL) {
	PUT_OBJVAL(&f->sp[2], obj);
    } else {
	PUT_LWOVAL(&f->sp[2], f->lwobj);
    }

    /* obj, stack, ticks */
    call_driver_object(f, "runtime_rlimits", 3);

    if (!VAL_TRUE(f->sp)) {
	error("Illegal use of rlimits");
    }
    i_del_value(f->sp++);
}

/*
 * NAME:	interpret->new_rlimits()
 * DESCRIPTION:	create new rlimits scope
 */
void i_new_rlimits(Frame *f, Int depth, Int t)
{
    rlinfo *rlim;

    rlim = ALLOC(rlinfo, 1);
    memset(rlim, '\0', sizeof(rlinfo));
    if (depth != 0) {
	if (depth < 0) {
	    rlim->nodepth = TRUE;
	} else {
	    rlim->maxdepth = f->depth + depth;
	    rlim->nodepth = FALSE;
	}
    } else {
	rlim->maxdepth = f->rlim->maxdepth;
	rlim->nodepth = f->rlim->nodepth;
    }
    if (t != 0) {
	if (t < 0) {
	    rlim->noticks = TRUE;
	} else {
	    t >>= f->level;
	    f->rlim->ticks -= t;
	    rlim->ticks = t;
	    rlim->noticks = FALSE;
	}
    } else {
	rlim->ticks = f->rlim->ticks;
	rlim->noticks = f->rlim->noticks;
	f->rlim->ticks = 0;
    }

    rlim->next = f->rlim;
    f->rlim = rlim;
}

/*
 * NAME:	interpret->set_rlimits()
 * DESCRIPTION:	restore rlimits to an earlier state
 */
void i_set_rlimits(Frame *f, rlinfo *rlim)
{
    rlinfo *r, *next;

    r = f->rlim;
    if (r->ticks < 0) {
	r->ticks = 0;
    }
    while (r != rlim) {
	next = r->next;
	if (!r->noticks) {
	    next->ticks += r->ticks;
	}
	FREE(r);
	r = next;
    }
    f->rlim = rlim;
}

/*
 * NAME:	interpret->set_sp()
 * DESCRIPTION:	set the current stack pointer
 */
Frame *i_set_sp(Frame *ftop, Value *sp)
{
    Value *v;
    Frame *f;

    for (f = ftop; ; f = f->prev) {
	v = f->sp;
	for (;;) {
	    if (v == sp) {
		f->sp = v;
		return f;
	    }
	    if (v == f->fp) {
		break;
	    }
	    switch (v->type) {
	    case T_STRING:
		str_del(v->u.string);
		break;

	    case T_ARRAY:
	    case T_MAPPING:
	    case T_LWOBJECT:
		arr_del(v->u.array);
		break;
	    }
	    v++;
	}

	if (f->lwobj != (Array *) NULL) {
	    arr_del(f->lwobj);
	}
	if (f->sos) {
	    /* stack on stack */
	    AFREE(f->stack);
	} else if (f->oindex != OBJ_NONE) {
	    FREE(f->stack);
	}
    }
}

/*
 * NAME:	interpret->prev_object()
 * DESCRIPTION:	return the nth previous object in the call_other chain
 */
Frame *i_prev_object(Frame *f, int n)
{
    while (n >= 0) {
	/* back to last external call */
	while (!f->external) {
	    f = f->prev;
	}
	f = f->prev;
	if (f->oindex == OBJ_NONE) {
	    return (Frame *) NULL;
	}
	--n;
    }
    return f;
}

/*
 * NAME:	interpret->prev_program()
 * DESCRIPTION:	return the nth previous program in the function call chain
 */
const char *i_prev_program(Frame *f, int n)
{
    while (n >= 0) {
	f = f->prev;
	if (f->oindex == OBJ_NONE) {
	    return (char *) NULL;
	}
	--n;
    }

    return OBJR(f->p_ctrl->oindex)->name;
}

/*
 * NAME:	interpret->typecheck()
 * DESCRIPTION:	check the argument types given to a function
 */
void i_typecheck(Frame *f, Frame *prog_f, const char *name, const char *ftype,
		 char *proto, int nargs, bool strict)
{
    char tnbuf[TNBUFSIZE];
    int i, n, atype, ptype;
    char *args;
    bool ellipsis;
    Uint sclass;
    Value *elts;

    sclass = 0;
    i = nargs;
    n = PROTO_NARGS(proto) + PROTO_VARGS(proto);
    ellipsis = ((PROTO_CLASS(proto) & C_ELLIPSIS) != 0);
    args = PROTO_ARGS(proto);
    while (n > 0 && i > 0) {
	--i;
	ptype = *args++;
	if ((ptype & T_TYPE) == T_CLASS) {
	    FETCH3U(args, sclass);
	}
	if (n == 1 && ellipsis) {
	    if (ptype == T_MIXED || ptype == T_LVALUE) {
		return;
	    }
	    if ((ptype & T_TYPE) == T_CLASS) {
		args -= 4;
	    } else {
		--args;
	    }
	} else {
	    --n;
	}

	if (ptype != T_MIXED) {
	    atype = f->sp[i].type;
	    if (atype == T_LWOBJECT) {
		atype = T_OBJECT;
	    }
	    if ((ptype & T_TYPE) == T_CLASS && ptype == T_CLASS &&
		atype == T_OBJECT) {
		if (f->sp[i].type == T_OBJECT) {
		    if (!i_instanceof(prog_f, f->sp[i].oindex, sclass)) {
			error("Bad object argument %d for function %s",
			      nargs - i, name);
		    }
		} else {
		    elts = d_get_elts(f->sp[i].u.array);
		    if (elts->type == T_OBJECT) {
			if (!i_instanceof(prog_f, elts->oindex, sclass)) {
			    error("Bad object argument %d for function %s",
				  nargs - i, name);
			}
		    } else if (strcmp(o_builtin_name(elts->u.number),
				      i_classname(prog_f, sclass)) != 0) {
			error("Bad object argument %d for function %s",
			      nargs - i, name);
		    }
		}
		continue;
	    }
	    if (ptype != atype && (atype != T_ARRAY || !(ptype & T_REF))) {
		if (!VAL_NIL(f->sp + i) || !T_POINTER(ptype)) {
		    /* wrong type */
		    error("Bad argument %d (%s) for %s %s", nargs - i,
			  i_typename(tnbuf, atype), ftype, name);
		} else if (strict) {
		    /* nil argument */
		    error("Bad argument %d for %s %s", nargs - i, ftype, name);
		}
	    }
	}
    }
}

/*
 * NAME:	interpret->switch_int()
 * DESCRIPTION:	handle an int switch
 */
static unsigned short i_switch_int(Frame *f, char *pc)
{
    unsigned short h, l, m, sz, dflt;
    Int num;
    char *p;

    FETCH2U(pc, h);
    sz = FETCH1U(pc);
    FETCH2U(pc, dflt);
    if (f->sp->type != T_INT) {
	return dflt;
    }

    l = 0;
    --h;
    switch (sz) {
    case 1:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 3 * m;
	    num = FETCH1S(p);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 2:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 4 * m;
	    FETCH2S(p, num);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 3:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 5 * m;
	    FETCH3S(p, num);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 4:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 6 * m;
	    FETCH4S(p, num);
	    if (f->sp->u.number == num) {
		return FETCH2U(p, l);
	    } else if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		l = m + 1;	/* search in upper half */
	    }
	}
	break;
    }

    return dflt;
}

/*
 * NAME:	interpret->switch_range()
 * DESCRIPTION:	handle a range switch
 */
static unsigned short i_switch_range(Frame *f, char *pc)
{
    unsigned short h, l, m, sz, dflt;
    Int num;
    char *p;

    FETCH2U(pc, h);
    sz = FETCH1U(pc);
    FETCH2U(pc, dflt);
    if (f->sp->type != T_INT) {
	return dflt;
    }

    l = 0;
    --h;
    switch (sz) {
    case 1:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 4 * m;
	    num = FETCH1S(p);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		num = FETCH1S(p);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 2:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 6 * m;
	    FETCH2S(p, num);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		FETCH2S(p, num);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 3:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 8 * m;
	    FETCH3S(p, num);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		FETCH3S(p, num);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;

    case 4:
	while (l < h) {
	    m = (l + h) >> 1;
	    p = pc + 10 * m;
	    FETCH4S(p, num);
	    if (f->sp->u.number < num) {
		h = m;	/* search in lower half */
	    } else {
		FETCH4S(p, num);
		if (f->sp->u.number <= num) {
		    return FETCH2U(p, l);
		}
		l = m + 1;	/* search in upper half */
	    }
	}
	break;
    }
    return dflt;
}

/*
 * NAME:	interpret->switch_str()
 * DESCRIPTION:	handle a string switch
 */
static unsigned short i_switch_str(Frame *f, char *pc)
{
    unsigned short h, l, m, u, u2, dflt;
    int cmp;
    char *p;
    Control *ctrl;

    FETCH2U(pc, h);
    FETCH2U(pc, dflt);
    if (FETCH1U(pc) == 0) {
	FETCH2U(pc, l);
	if (VAL_NIL(f->sp)) {
	    return l;
	}
	--h;
    }
    if (f->sp->type != T_STRING) {
	return dflt;
    }

    ctrl = f->p_ctrl;
    l = 0;
    --h;
    while (l < h) {
	m = (l + h) >> 1;
	p = pc + 5 * m;
	u = FETCH1U(p);
	cmp = str_cmp(f->sp->u.string, d_get_strconst(ctrl, u, FETCH2U(p, u2)));
	if (cmp == 0) {
	    return FETCH2U(p, l);
	} else if (cmp < 0) {
	    h = m;	/* search in lower half */
	} else {
	    l = m + 1;	/* search in upper half */
	}
    }
    return dflt;
}

/*
 * NAME:	interpret->catcherr()
 * DESCRIPTION:	handle caught error
 */
void i_catcherr(Frame *f, Int depth)
{
    i_runtime_error(f, depth);
}

/*
 * NAME:	interpret->interpret1()
 * DESCRIPTION:	Main interpreter function v1. Interpret stack machine code.
 */
static void i_interpret1(Frame *f, char *pc)
{
    unsigned short instr, u, u2;
    Uint l;
    char *p;
    kfunc *kf;
    int size, instance;
    bool atomic;
    Int newdepth, newticks;
    Value val;

    size = 0;
    l = 0;

    for (;;) {
# ifdef DEBUG
	if (f->sp < f->stack + MIN_STACK) {
	    fatal("out of value stack");
	}
# endif
	if (--f->rlim->ticks <= 0) {
	    if (f->rlim->noticks) {
		f->rlim->ticks = 0x7fffffff;
	    } else {
		error("Out of ticks");
	    }
	}
	instr = FETCH1U(pc);
	f->pc = pc;

	switch (instr & I_INSTR_MASK) {
	case I_PUSH_INT1:
	    PUSH_INTVAL(f, FETCH1S(pc));
	    continue;

	case I_PUSH_INT2:
	    PUSH_INTVAL(f, FETCH2S(pc, u));
	    continue;

	case I_PUSH_INT4:
	    PUSH_INTVAL(f, FETCH4S(pc, l));
	    continue;

	case I_PUSH_FLOAT6:
	    FETCH2U(pc, u);
	    PUSH_FLTCONST(f, u, FETCH4U(pc, l));
	    continue;

	case I_PUSH_STRING:
	    PUSH_STRVAL(f, d_get_strconst(f->p_ctrl, f->p_ctrl->ninherits - 1,
					  FETCH1U(pc)));
	    continue;

	case I_PUSH_NEAR_STRING:
	    u = FETCH1U(pc);
	    PUSH_STRVAL(f, d_get_strconst(f->p_ctrl, u, FETCH1U(pc)));
	    continue;

	case I_PUSH_FAR_STRING:
	    u = FETCH1U(pc);
	    PUSH_STRVAL(f, d_get_strconst(f->p_ctrl, u, FETCH2U(pc, u2)));
	    continue;

	case I_PUSH_LOCAL:
	    u = FETCH1S(pc);
	    i_push_value(f, ((short) u < 0) ? f->fp + (short) u : f->argp + u);
	    continue;

	case I_PUSH_GLOBAL:
	    i_global(f, f->p_ctrl->ninherits - 1, FETCH1U(pc));
	    continue;

	case I_PUSH_FAR_GLOBAL:
	    u = FETCH1U(pc);
	    i_global(f, u, FETCH1U(pc));
	    continue;

	case I_INDEX:
	case I_INDEX | I_POP_BIT:
	    i_index2(f, f->sp + 1, f->sp, &val, FALSE);
	    *++f->sp = val;
	    break;

	case I_INDEX2:
	    i_index2(f, f->sp + 1, f->sp, &val, TRUE);
	    *--f->sp = val;
	    continue;

	case I_AGGREGATE:
	case I_AGGREGATE | I_POP_BIT:
	    if (FETCH1U(pc) == 0) {
		i_aggregate(f, FETCH2U(pc, u));
	    } else {
		i_map_aggregate(f, FETCH2U(pc, u));
	    }
	    break;

	case I_SPREAD:
	    u = FETCH1S(pc);
	    size = i_spread1(f, -(short) u - 2);
	    continue;

	case I_CAST:
	case I_CAST | I_POP_BIT:
	    u = FETCH1U(pc);
	    if (u == T_CLASS) {
		FETCH3U(pc, l);
	    }
	    i_cast(f, f->sp, u, l);
	    break;

	case I_INSTANCEOF:
	case I_INSTANCEOF | I_POP_BIT:
	    FETCH3U(pc, l);
	    switch (f->sp->type) {
	    case T_OBJECT:
		instance = i_instanceof(f, f->sp->oindex, l);
		break;

	    case T_LWOBJECT:
		if (f->sp->u.array->elts->type != T_OBJECT) {
		    instance =
			(strcmp(o_builtin_name(f->sp->u.array->elts->u.number),
				i_classname(f, l)) == 0);
		} else {
		    instance = i_instanceof(f, f->sp->u.array->elts->oindex, l);
		}
		arr_del(f->sp->u.array);
		break;

	    default:
		error("Instance of bad type");
	    }

	    PUT_INTVAL(f->sp, instance);
	    break;

	case I_STORES:
	    u = FETCH1U(pc);
	    if (f->sp->type != T_ARRAY || u > f->sp->u.array->size) {
		error("Wrong number of lvalues");
	    }
	    d_get_elts(f->sp->u.array);
	    f->pc = pc;
	    i_stores(f, 0, u);
	    pc = f->pc;
	    continue;

	case I_STORE_LOCAL:
	case I_STORE_LOCAL | I_POP_BIT:
	    i_store_local(f, FETCH1S(pc), f->sp, NULL);
	    break;

	case I_STORE_GLOBAL:
	case I_STORE_GLOBAL | I_POP_BIT:
	    i_store_global(f, f->p_ctrl->ninherits - 1, FETCH1U(pc), f->sp,
			   NULL);
	    break;

	case I_STORE_FAR_GLOBAL:
	case I_STORE_FAR_GLOBAL | I_POP_BIT:
	    u = FETCH1U(pc);
	    i_store_global(f, u, FETCH1U(pc), f->sp, NULL);
	    break;

	case I_STORE_INDEX:
	case I_STORE_INDEX | I_POP_BIT:
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1, f->sp)) {
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_LOCAL_INDEX:
	case I_STORE_LOCAL_INDEX | I_POP_BIT:
	    u = FETCH1S(pc);
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1, f->sp)) {
		i_store_local(f, (short) u, &val, f->sp + 2);
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_GLOBAL_INDEX:
	case I_STORE_GLOBAL_INDEX | I_POP_BIT:
	    u = FETCH1U(pc);
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1, f->sp)) {
		i_store_global(f, f->p_ctrl->ninherits - 1, u, &val, f->sp + 2);
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_FAR_GLOBAL_INDEX:
	case I_STORE_FAR_GLOBAL_INDEX | I_POP_BIT:
	    u = FETCH1U(pc);
	    u2 = FETCH1U(pc);
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1, f->sp)) {
		i_store_global(f, u, u2, &val, f->sp + 2);
		str_del(f->sp[2].u.string);
		str_del(val.u.string);
	    }
	    f->sp[2] = f->sp[0];
	    f->sp += 2;
	    break;

	case I_STORE_INDEX_INDEX:
	case I_STORE_INDEX_INDEX | I_POP_BIT:
	    val = nil_value;
	    if (i_store_index(f, &val, f->sp + 2, f->sp + 1, f->sp)) {
		f->sp[1] = val;
		i_store_index(f, f->sp + 2, f->sp + 4, f->sp + 3, f->sp + 1);
		str_del(f->sp[1].u.string);
		str_del(f->sp[2].u.string);
	    } else {
		i_del_value(f->sp + 3);
		i_del_value(f->sp + 4);
	    }
	    f->sp[4] = f->sp[0];
	    f->sp += 4;
	    break;

	case I_JUMP_ZERO:
	    p = f->prog + FETCH2U(pc, u);
	    if (!VAL_TRUE(f->sp)) {
		pc = p;
	    }
	    i_del_value(f->sp++);
	    continue;

	case I_JUMP_NONZERO:
	    p = f->prog + FETCH2U(pc, u);
	    if (VAL_TRUE(f->sp)) {
		pc = p;
	    }
	    i_del_value(f->sp++);
	    continue;

	case I_JUMP:
	    p = f->prog + FETCH2U(pc, u);
	    pc = p;
	    continue;

	case I_SWITCH:
	    switch (FETCH1U(pc)) {
	    case SWITCH_INT:
		pc = f->prog + i_switch_int(f, pc);
		break;

	    case SWITCH_RANGE:
		pc = f->prog + i_switch_range(f, pc);
		break;

	    case SWITCH_STRING:
		pc = f->prog + i_switch_str(f, pc);
		break;
	    }
	    i_del_value(f->sp++);
	    continue;

	case I_CALL_KFUNC:
	case I_CALL_KFUNC | I_POP_BIT:
	    kf = &KFUN(FETCH1U(pc));
	    if (PROTO_VARGS(kf->proto) != 0) {
		/* variable # of arguments */
		u = FETCH1U(pc) + size;
		size = 0;
	    } else {
		/* fixed # of arguments */
		u = PROTO_NARGS(kf->proto);
	    }
	    if (PROTO_CLASS(kf->proto) & C_TYPECHECKED) {
		i_typecheck(f, (Frame *) NULL, kf->name, "kfun", kf->proto, u,
			    TRUE);
	    }
	    f->pc = pc;
	    u = (*kf->func)(f, u, kf);
	    if (u != 0) {
		if ((short) u < 0) {
		    error("Too few arguments for kfun %s", kf->name);
		} else if (u <= PROTO_NARGS(kf->proto) + PROTO_VARGS(kf->proto))
		{
		    error("Bad argument %d for kfun %s", u, kf->name);
		} else {
		    error("Too many arguments for kfun %s", kf->name);
		}
	    }
	    pc = f->pc;
	    break;

	case I_CALL_EFUNC:
	case I_CALL_EFUNC | I_POP_BIT:
	    kf = &KFUN(FETCH2U(pc, u));
	    if (PROTO_VARGS(kf->proto) != 0) {
		/* variable # of arguments */
		u = FETCH1U(pc) + size;
		size = 0;
	    } else {
		/* fixed # of arguments */
		u = PROTO_NARGS(kf->proto);
	    }
	    if (PROTO_CLASS(kf->proto) & C_TYPECHECKED) {
		i_typecheck(f, (Frame *) NULL, kf->name, "kfun", kf->proto, u,
			    TRUE);
	    }
	    f->pc = pc;
	    u = (*kf->func)(f, u, kf);
	    if (u != 0) {
		if ((short) u < 0) {
		    error("Too few arguments for kfun %s", kf->name);
		} else if (u <= PROTO_NARGS(kf->proto) + PROTO_VARGS(kf->proto))
		{
		    error("Bad argument %d for kfun %s", u, kf->name);
		} else {
		    error("Too many arguments for kfun %s", kf->name);
		}
	    }
	    pc = f->pc;
	    break;

	case I_CALL_CKFUNC:
	case I_CALL_CKFUNC | I_POP_BIT:
	    kf = &KFUN(FETCH1U(pc));
	    u = FETCH1U(pc) + size;
	    size = 0;
	    if (u != PROTO_NARGS(kf->proto)) {
		if (u < PROTO_NARGS(kf->proto)) {
		    error("Too few arguments for kfun %s", kf->name);
		} else {
		    error("Too many arguments for kfun %s", kf->name);
		}
	    }
	    if (PROTO_CLASS(kf->proto) & C_TYPECHECKED) {
		i_typecheck(f, (Frame *) NULL, kf->name, "kfun", kf->proto, u,
			    TRUE);
	    }
	    f->pc = pc;
	    u = (*kf->func)(f, u, kf);
	    if (u != 0) {
		error("Bad argument %d for kfun %s", u, kf->name);
	    }
	    pc = f->pc;
	    break;

	case I_CALL_CEFUNC:
	case I_CALL_CEFUNC | I_POP_BIT:
	    kf = &KFUN(FETCH2U(pc, u));
	    u = FETCH1U(pc) + size;
	    size = 0;
	    if (u != PROTO_NARGS(kf->proto)) {
		if (u < PROTO_NARGS(kf->proto)) {
		    error("Too few arguments for kfun %s", kf->name);
		} else {
		    error("Too many arguments for kfun %s", kf->name);
		}
	    }
	    if (PROTO_CLASS(kf->proto) & C_TYPECHECKED) {
		i_typecheck(f, (Frame *) NULL, kf->name, "kfun", kf->proto, u,
			    TRUE);
	    }
	    f->pc = pc;
	    u = (*kf->func)(f, u, kf);
	    if (u != 0) {
		error("Bad argument %d for kfun %s", u, kf->name);
	    }
	    pc = f->pc;
	    break;

	case I_CALL_AFUNC:
	case I_CALL_AFUNC | I_POP_BIT:
	    u = FETCH1U(pc);
	    i_funcall(f, (Object *) NULL, (Array *) NULL, 0, u,
		      FETCH1U(pc) + size);
	    size = 0;
	    break;

	case I_CALL_DFUNC:
	case I_CALL_DFUNC | I_POP_BIT:
	    u = FETCH1U(pc);
	    u2 = FETCH1U(pc);
	    i_funcall(f, (Object *) NULL, (Array *) NULL,
		      UCHAR(f->ctrl->imap[f->p_index + u]), u2,
		      FETCH1U(pc) + size);
	    size = 0;
	    break;

	case I_CALL_FUNC:
	case I_CALL_FUNC | I_POP_BIT:
	    p = &f->ctrl->funcalls[2L * (f->foffset + FETCH2U(pc, u))];
	    i_funcall(f, (Object *) NULL, (Array *) NULL, UCHAR(p[0]),
		      UCHAR(p[1]), FETCH1U(pc) + size);
	    size = 0;
	    break;

	case I_CATCH:
	case I_CATCH | I_POP_BIT:
	    atomic = f->atomic;
	    p = f->prog + FETCH2U(pc, u);
	    try {
		ec_push((ec_ftn) i_catcherr);
		f->atomic = FALSE;
		i_interpret1(f, pc);
		ec_pop();
		pc = f->pc;
		*--f->sp = nil_value;
	    } catch (...) {
		/* error */
		f->pc = pc = p;
		PUSH_STRVAL(f, errorstr());
	    }
	    f->atomic = atomic;
	    break;

	case I_RLIMITS:
	    if (f->sp[1].type != T_INT) {
		error("Bad rlimits depth type");
	    }
	    if (f->sp->type != T_INT) {
		error("Bad rlimits ticks type");
	    }
	    newdepth = f->sp[1].u.number;
	    newticks = f->sp->u.number;
	    if (!FETCH1U(pc)) {
		/* runtime check */
		i_check_rlimits(f);
	    } else {
		/* pop limits */
		f->sp += 2;
	    }
	    i_new_rlimits(f, newdepth, newticks);
	    i_interpret1(f, pc);
	    pc = f->pc;
	    i_set_rlimits(f, f->rlim->next);
	    continue;

	case I_RETURN:
	    return;

# ifdef DEBUG
	default:
	    fatal("illegal instruction");
# endif
	}

	if (instr & I_POP_BIT) {
	    /* pop the result of the last operation (never an lvalue) */
	    i_del_value(f->sp++);
	}
    }
}

/*
 * NAME:	interpret->funcall()
 * DESCRIPTION:	Call a function in an object. The arguments must be on the
 *		stack already.
 */
void i_funcall(Frame *prev_f, Object *obj, Array *lwobj, int p_ctrli, int funci, int nargs)
{
    char *pc;
    unsigned short n;
    Frame f;
    bool ellipsis;
    Value val;

    f.prev = prev_f;
    if (prev_f->oindex == OBJ_NONE) {
	/*
	 * top level call
	 */
	f.oindex = obj->index;
	f.lwobj = (Array *) NULL;
	f.ctrl = obj->ctrl;
	f.data = o_dataspace(obj);
	f.external = TRUE;
    } else if (lwobj != (Array *) NULL) {
	/*
	 * call_other to lightweight object
	 */
	f.oindex = obj->index;
	f.lwobj = lwobj;
	f.ctrl = obj->ctrl;
	f.data = lwobj->primary->data;
	f.external = TRUE;
    } else if (obj != (Object *) NULL) {
	/*
	 * call_other to persistent object
	 */
	f.oindex = obj->index;
	f.lwobj = (Array *) NULL;
	f.ctrl = obj->ctrl;
	f.data = o_dataspace(obj);
	f.external = TRUE;
    } else {
	/*
	 * local function call
	 */
	f.oindex = prev_f->oindex;
	f.lwobj = prev_f->lwobj;
	f.ctrl = prev_f->ctrl;
	f.data = prev_f->data;
	f.external = FALSE;
    }
    f.depth = prev_f->depth + 1;
    f.rlim = prev_f->rlim;
    if (f.depth >= f.rlim->maxdepth && !f.rlim->nodepth) {
	error("Stack overflow");
    }
    if (f.rlim->ticks < 100) {
	if (f.rlim->noticks) {
	    f.rlim->ticks = 0x7fffffff;
	} else {
	    error("Out of ticks");
	}
    }

    /* set the program control block */
    obj = OBJR(f.ctrl->inherits[p_ctrli].oindex);
    f.foffset = f.ctrl->inherits[p_ctrli].funcoffset;
    f.p_ctrl = o_control(obj);
    f.p_index = f.ctrl->inherits[p_ctrli].progoffset;

    /* get the function */
    f.func = &d_get_funcdefs(f.p_ctrl)[funci];
    if (f.func->sclass & C_UNDEFINED) {
	error("Undefined function %s",
	      d_get_strconst(f.p_ctrl, f.func->inherit, f.func->index)->text);
    }

    pc = d_get_prog(f.p_ctrl) + f.func->offset;
    if (f.func->sclass & C_TYPECHECKED) {
	/* typecheck arguments */
	i_typecheck(prev_f, &f,
		    d_get_strconst(f.p_ctrl, f.func->inherit,
				   f.func->index)->text,
		    "function", pc, nargs, FALSE);
    }

    /* handle arguments */
    ellipsis = ((PROTO_CLASS(pc) & C_ELLIPSIS) != 0);
    n = PROTO_NARGS(pc) + PROTO_VARGS(pc);
    if (nargs < n) {
	int i;

	/* if fewer actual than formal parameters, check for varargs */
	if (nargs < PROTO_NARGS(pc) && stricttc) {
	    error("Insufficient arguments for function %s",
		  d_get_strconst(f.p_ctrl, f.func->inherit,
				 f.func->index)->text);
	}

	/* add missing arguments */
	i_grow_stack(prev_f, n - nargs);
	if (ellipsis) {
	    --n;
	}

	pc = &PROTO_FTYPE(pc);
	i = nargs;
	do {
	    if ((FETCH1U(pc) & T_TYPE) == T_CLASS) {
		pc += 3;
	    }
	} while (--i >= 0);
	while (nargs < n) {
	    switch (i=FETCH1U(pc)) {
	    case T_INT:
		*--prev_f->sp = zero_int;
		break;

	    case T_FLOAT:
		*--prev_f->sp = zero_float;
		    break;

	    default:
		if ((i & T_TYPE) == T_CLASS) {
		    pc += 3;
		}
		*--prev_f->sp = nil_value;
		break;
	    }
	    nargs++;
	}
	if (ellipsis) {
	    PUSH_ARRVAL(prev_f, arr_new(f.data, 0));
	    nargs++;
	    if ((FETCH1U(pc) & T_TYPE) == T_CLASS) {
		pc += 3;
	    }
	}
    } else if (ellipsis) {
	Value *v;
	Array *a;

	/* put additional arguments in array */
	nargs -= n - 1;
	a = arr_new(f.data, nargs);
	v = a->elts + nargs;
	do {
	    *--v = *prev_f->sp++;
	} while (--nargs > 0);
	d_ref_imports(a);
	PUSH_ARRVAL(prev_f, a);
	nargs = n;
	pc += PROTO_SIZE(pc);
    } else if (nargs > n) {
	if (stricttc) {
	    error("Too many arguments for function %s",
		  d_get_strconst(f.p_ctrl, f.func->inherit,
				 f.func->index)->text);
	}

	/* pop superfluous arguments */
	i_pop(prev_f, nargs - n);
	nargs = n;
	pc += PROTO_SIZE(pc);
    } else {
	pc += PROTO_SIZE(pc);
    }
    f.sp = prev_f->sp;
    f.nargs = nargs;
    cframe = &f;
    if (f.lwobj != (Array *) NULL) {
	arr_ref(f.lwobj);
    }

    /* deal with atomic functions */
    f.level = prev_f->level;
    if ((f.func->sclass & C_ATOMIC) && !prev_f->atomic) {
	o_new_plane();
	d_new_plane(f.data, ++f.level);
	f.atomic = TRUE;
	if (!f.rlim->noticks) {
	    f.rlim->ticks >>= 1;
	}
    } else {
	if (f.level != f.data->plane->level) {
	    d_new_plane(f.data, f.level);
	}
	f.atomic = prev_f->atomic;
    }

    i_add_ticks(&f, 10);

    /* create new local stack */
    f.argp = f.sp;
    FETCH2U(pc, n);
    f.stack = ALLOCA(Value, n + MIN_STACK + EXTRA_STACK);
    f.fp = f.sp = f.stack + n + MIN_STACK + EXTRA_STACK;
    f.sos = TRUE;

    /* initialize local variables */
    n = FETCH1U(pc);
# ifdef DEBUG
    nargs = n;
# endif
    if (n > 0) {
	do {
	    *--f.sp = nil_value;
	} while (--n > 0);
    }

    /* execute code */
    d_get_funcalls(f.ctrl);	/* make sure they are available */
    f.prog = pc += 2;
    i_interpret1(&f, pc);

    /* clean up stack, move return value to outer stackframe */
    val = *f.sp++;
# ifdef DEBUG
    if (f.sp != f.fp - nargs) {
	fatal("bad stack pointer after function call");
    }
# endif
    i_pop(&f, f.fp - f.sp);
    if (f.sos) {
	    /* still alloca'd */
	AFREE(f.stack);
    } else {
	/* extended and malloced */
	FREE(f.stack);
    }

    if (f.lwobj != (Array *) NULL) {
	arr_del(f.lwobj);
    }
    cframe = prev_f;
    i_pop(prev_f, f.nargs);
    *--prev_f->sp = val;

    if ((f.func->sclass & C_ATOMIC) && !prev_f->atomic) {
	d_commit_plane(f.level, &val);
	o_commit_plane();
	if (!f.rlim->noticks) {
	    f.rlim->ticks *= 2;
	}
    }
}

/*
 * NAME:	interpret->call()
 * DESCRIPTION:	Attempt to call a function in an object. Return TRUE if
 *		the call succeeded.
 */
bool i_call(Frame *f, Object *obj, Array *lwobj, const char *func,
	    unsigned int len, int call_static, int nargs)
{
    dsymbol *symb;
    dfuncdef *fdef;
    Control *ctrl;

    if (lwobj != (Array *) NULL) {
	uindex oindex;
	Float flt;
	Value val;

	GET_FLT(&lwobj->elts[1], flt);
	if (lwobj->elts[0].type == T_OBJECT) {
	    /*
	     * ordinary light-weight object: upgrade first if needed
	     */
	    oindex = lwobj->elts[0].oindex;
	    obj = OBJR(oindex);
	    if (obj->update != flt.low) {
		d_upgrade_lwobj(lwobj, obj);
	    }
	}
	if (flt.high != FALSE) {
	    /*
	     * touch the light-weight object
	     */
	    flt.high = FALSE;
	    PUT_FLTVAL(&val, flt);
	    d_assign_elt(f->data, lwobj, &lwobj->elts[1], &val);
	    PUSH_LWOVAL(f, lwobj);
	    PUSH_STRVAL(f, str_new(func, len));
	    call_driver_object(f, "touch", 2);
	    if (VAL_TRUE(f->sp)) {
		/* preserve through call */
		flt.high = TRUE;
		PUT_FLT(&lwobj->elts[1], flt);
	    }
	    i_del_value(f->sp++);
	}
	if (lwobj->elts[0].type == T_INT) {
	    /* no user-callable functions within (right?) */
	    i_pop(f, nargs);
	    return FALSE;
	}
    } else if (!(obj->flags & O_TOUCHED)) {
	/*
	 * initialize/touch the object
	 */
	obj = OBJW(obj->index);
	obj->flags |= O_TOUCHED;
	if (O_HASDATA(obj)) {
	    PUSH_OBJVAL(f, obj);
	    PUSH_STRVAL(f, str_new(func, len));
	    call_driver_object(f, "touch", 2);
	    if (VAL_TRUE(f->sp)) {
		obj->flags &= ~O_TOUCHED;	/* preserve though call */
	    }
	    i_del_value(f->sp++);
	} else {
	    obj->data = d_new_dataspace(obj);
	    if (func != (char *) NULL &&
		i_call(f, obj, (Array *) NULL, creator, clen, TRUE, 0)) {
		i_del_value(f->sp++);
	    }
	}
    }
    if (func == (char *) NULL) {
	func = creator;
	len = clen;
    }

    /* find the function in the symbol table */
    ctrl = o_control(obj);
    symb = ctrl_symb(ctrl, func, len);
    if (symb == (dsymbol *) NULL) {
	/* function doesn't exist in symbol table */
	i_pop(f, nargs);
	return FALSE;
    }

    ctrl = OBJR(ctrl->inherits[UCHAR(symb->inherit)].oindex)->ctrl;
    fdef = &d_get_funcdefs(ctrl)[UCHAR(symb->index)];

    /* check if the function can be called */
    if (!call_static && (fdef->sclass & C_STATIC) &&
	(f->oindex != obj->index || f->lwobj != lwobj)) {
	i_pop(f, nargs);
	return FALSE;
    }

    /* call the function */
    i_funcall(f, obj, lwobj, UCHAR(symb->inherit), UCHAR(symb->index), nargs);

    return TRUE;
}

/*
 * NAME:	interpret->line1()
 * DESCRIPTION:	return the line number the program counter of the specified
 *		frame is at
 */
static unsigned short i_line1(Frame *f)
{
    char *pc, *numbers;
    int instr;
    short offset;
    unsigned short line, u, sz;

    line = 0;
    pc = f->p_ctrl->prog + f->func->offset;
    pc += PROTO_SIZE(pc) + 3;
    FETCH2U(pc, u);
    numbers = pc + u;

    while (pc < f->pc) {
	instr = FETCH1U(pc);

	offset = instr >> I_LINE_SHIFT;
	if (offset <= 2) {
	    /* simple offset */
	    line += offset;
	} else {
	    offset = FETCH1U(numbers);
	    if (offset >= 128) {
		/* one byte offset */
		line += offset - 128 - 64;
	    } else {
		/* two byte offset */
		line += ((offset << 8) | FETCH1U(numbers)) - 16384;
	    }
	}

	switch (instr & I_INSTR_MASK) {
	case I_INDEX:
	case I_INDEX | I_POP_BIT:
	case I_INDEX2:
	case I_STORE_INDEX:
	case I_STORE_INDEX | I_POP_BIT:
	case I_STORE_INDEX_INDEX:
	case I_STORE_INDEX_INDEX | I_POP_BIT:
	case I_RETURN:
	    break;

	case I_CALL_KFUNC:
	case I_CALL_KFUNC | I_POP_BIT:
	    if (PROTO_VARGS(KFUN(FETCH1U(pc)).proto) != 0) {
		pc++;
	    }
	    break;

	case I_PUSH_INT1:
	case I_PUSH_STRING:
	case I_PUSH_LOCAL:
	case I_PUSH_GLOBAL:
	case I_STORE_LOCAL:
	case I_STORE_LOCAL | I_POP_BIT:
	case I_STORE_GLOBAL:
	case I_STORE_GLOBAL | I_POP_BIT:
	case I_STORES:
	case I_STORE_LOCAL_INDEX:
	case I_STORE_LOCAL_INDEX | I_POP_BIT:
	case I_STORE_GLOBAL_INDEX:
	case I_STORE_GLOBAL_INDEX | I_POP_BIT:
	case I_RLIMITS:
	    pc++;
	    break;

	case I_SPREAD:
	    if (FETCH1S(pc) < 0) {
		break;
	    }
	    /* fall through */
	case I_CAST:
	case I_CAST | I_POP_BIT:
	    if (FETCH1U(pc) == T_CLASS) {
		pc += 3;
	    }
	    break;

	case I_CALL_EFUNC:
	case I_CALL_EFUNC | I_POP_BIT:
	    if (PROTO_VARGS(KFUN(FETCH2U(pc, u)).proto) != 0) {
		pc++;
	    }
	    break;

	case I_PUSH_INT2:
	case I_PUSH_NEAR_STRING:
	case I_PUSH_FAR_GLOBAL:
	case I_STORE_FAR_GLOBAL:
	case I_STORE_FAR_GLOBAL | I_POP_BIT:
	case I_STORE_FAR_GLOBAL_INDEX:
	case I_STORE_FAR_GLOBAL_INDEX | I_POP_BIT:
	case I_JUMP_ZERO:
	case I_JUMP_NONZERO:
	case I_JUMP:
	case I_CALL_AFUNC:
	case I_CALL_AFUNC | I_POP_BIT:
	case I_CALL_CKFUNC:
	case I_CALL_CKFUNC | I_POP_BIT:
	case I_CATCH:
	case I_CATCH | I_POP_BIT:
	    pc += 2;
	    break;

	case I_PUSH_FAR_STRING:
	case I_AGGREGATE:
	case I_AGGREGATE | I_POP_BIT:
	case I_INSTANCEOF:
	case I_INSTANCEOF | I_POP_BIT:
	case I_CALL_DFUNC:
	case I_CALL_DFUNC | I_POP_BIT:
	case I_CALL_FUNC:
	case I_CALL_FUNC | I_POP_BIT:
	case I_CALL_CEFUNC:
	case I_CALL_CEFUNC | I_POP_BIT:
	    pc += 3;
	    break;

	case I_PUSH_INT4:
	    pc += 4;
	    break;

	case I_PUSH_FLOAT6:
	    pc += 6;
	    break;

	case I_SWITCH:
	    switch (FETCH1U(pc)) {
	    case 0:
		FETCH2U(pc, u);
		sz = FETCH1U(pc);
		pc += 2 + (u - 1) * (sz + 2);
		break;

	    case 1:
		FETCH2U(pc, u);
		sz = FETCH1U(pc);
		pc += 2 + (u - 1) * (2 * sz + 2);
		break;

	    case 2:
		FETCH2U(pc, u);
		pc += 2;
		if (FETCH1U(pc) == 0) {
		    pc += 2;
		    --u;
		}
		pc += (u - 1) * 5;
		break;
	    }
	    break;
	}
    }

    return line;
}

/*
 * NAME:	interpret->func_trace()
 * DESCRIPTION:	return the trace of a single function
 */
static Array *i_func_trace(Frame *f, Dataspace *data)
{
    char buffer[STRINGSZ + 12];
    Value *v;
    String *str;
    const char *name;
    unsigned short n;
    Value *args;
    Array *a;
    unsigned short max_args;

    max_args = conf_array_size() - 5;

    n = f->nargs;
    args = f->argp + n;
    if (n > max_args) {
	/* unlikely, but possible */
	n = max_args;
    }
    a = arr_new(data, n + 5L);
    v = a->elts;

    /* object name */
    name = o_name(buffer, OBJR(f->oindex));
    if (f->lwobj == (Array *) NULL) {
	PUT_STRVAL(v, str = str_new((char *) NULL, strlen(name) + 1L));
	v++;
	str->text[0] = '/';
	strcpy(str->text + 1, name);
    } else {
	PUT_STRVAL(v, str = str_new((char *) NULL, strlen(name) + 4L));
	v++;
	str->text[0] = '/';
	strcpy(str->text + 1, name);
	strcpy(str->text + str->len - 3, "#-1");
    }

    /* program name */
    name = OBJR(f->p_ctrl->oindex)->name;
    PUT_STRVAL(v, str = str_new((char *) NULL, strlen(name) + 1L));
    v++;
    str->text[0] = '/';
    strcpy(str->text + 1, name);

    /* function name */
    PUT_STRVAL(v, d_get_strconst(f->p_ctrl, f->func->inherit, f->func->index));
    v++;

    /* line number */
    PUT_INTVAL(v, i_line1(f));
    v++;

    /* external flag */
    PUT_INTVAL(v, f->external);
    v++;

    /* arguments */
    while (n > 0) {
	*v++ = *--args;
	i_ref_value(args);
	--n;
    }
    d_ref_imports(a);

    return a;
}

/*
 * NAME:	interpret->call_tracei()
 * DESCRIPTION:	get the trace of a single function
 */
bool i_call_tracei(Frame *ftop, Int idx, Value *v)
{
    Frame *f;
    unsigned short n;

    for (f = ftop, n = 0; f->oindex != OBJ_NONE; f = f->prev, n++) ;
    if (idx < 0 || idx >= n) {
	return FALSE;
    }

    for (f = ftop, n -= idx + 1; n != 0; f = f->prev, --n) ;
    PUT_ARRVAL(v, i_func_trace(f, ftop->data));
    return TRUE;
}

/*
 * NAME:	interpret->call_trace()
 * DESCRIPTION:	return the function call trace
 */
Array *i_call_trace(Frame *ftop)
{
    Frame *f;
    Value *v;
    unsigned short n;
    Array *a;

    for (f = ftop, n = 0; f->oindex != OBJ_NONE; f = f->prev, n++) ;
    a = arr_new(ftop->data, (long) n);
    i_add_ticks(ftop, 10 * n);
    for (f = ftop, v = a->elts + n; f->oindex != OBJ_NONE; f = f->prev) {
	--v;
	PUT_ARRVAL(v, i_func_trace(f, ftop->data));
    }

    return a;
}

/*
 * NAME:	emptyhandler()
 * DESCRIPTION:	fake error handler
 */
static void emptyhandler(Frame *f, Int depth)
{
    UNREFERENCED_PARAMETER(f);
    UNREFERENCED_PARAMETER(depth);
}

/*
 * NAME:	interpret->call_critical()
 * DESCRIPTION:	Call a function in the driver object at a critical moment.
 *		The function is called with rlimits (-1; -1) and errors
 *		caught.
 */
bool i_call_critical(Frame *f, const char *func, int narg, int flag)
{
    bool ok;

    i_new_rlimits(f, -1, -1);
    f->sp += narg;		/* so the error context knows what to pop */
    try {
	ec_push((flag) ? (ec_ftn) NULL : (ec_ftn) emptyhandler);
	f->sp -= narg;	/* recover arguments */
	call_driver_object(f, func, narg);
	ok = TRUE;
	ec_pop();
    } catch (...) {
	ok = FALSE;
    }
    i_set_rlimits(f, f->rlim->next);

    return ok;
}

/*
 * NAME:	interpret->runtime_error()
 * DESCRIPTION:	handle a runtime error
 */
void i_runtime_error(Frame *f, Int depth)
{
    PUSH_STRVAL(f, errorstr());
    PUSH_INTVAL(f, depth);
    PUSH_INTVAL(f, i_get_ticks(f));
    if (!i_call_critical(f, "runtime_error", 3, FALSE)) {
	message("Error within runtime_error:\012");	/* LF */
	message((char *) NULL);
    } else {
	if (f->sp->type == T_STRING) {
	    set_errorstr(f->sp->u.string);
	}
	i_del_value(f->sp++);
    }
}

/*
 * NAME:	interpret->atomic_error()
 * DESCRIPTION:	handle error in atomic code
 */
void i_atomic_error(Frame *ftop, Int level)
{
    Frame *f;

    for (f = ftop; f->level != level; f = f->prev) ;

    PUSH_STRVAL(ftop, errorstr());
    PUSH_INTVAL(ftop, f->depth);
    PUSH_INTVAL(ftop, i_get_ticks(ftop));
    if (!i_call_critical(ftop, "atomic_error", 3, FALSE)) {
	message("Error within atomic_error:\012");	/* LF */
	message((char *) NULL);
    } else {
	if (ftop->sp->type == T_STRING) {
	    set_errorstr(ftop->sp->u.string);
	}
	i_del_value(ftop->sp++);
    }
}

/*
 * NAME:	interpret->restore()
 * DESCRIPTION:	restore state to given level
 */
Frame *i_restore(Frame *ftop, Int level)
{
    Frame *f;

    for (f = ftop; f->level != level; f = f->prev) ;

    if (f->rlim != ftop->rlim) {
	i_set_rlimits(ftop, f->rlim);
    }
    if (!f->rlim->noticks) {
	f->rlim->ticks *= 2;
    }
    i_set_sp(ftop, f->sp);
    d_discard_plane(ftop->level);
    o_discard_plane();

    return f;
}

/*
 * NAME:	interpret->clear()
 * DESCRIPTION:	clean up the interpreter state
 */
void i_clear()
{
    Frame *f;

    f = cframe;
    if (f->stack != stack) {
	FREE(f->stack);
	f->fp = f->sp = stack + MIN_STACK;
	f->stack = stack;
    }

    f->rlim = &rlim;
}
