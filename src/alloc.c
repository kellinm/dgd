# include "dgd.h"

# ifndef STRUCT_AL
# define STRUCT_AL	2		/* default memory alignment */
# endif

# define MAGIC_MASK	0xff000000L	/* magic number mask */
# define SIZE_MASK	0x00ffffffL	/* size mask */

# define SM_MAGIC	0xc5000000L	/* static mem */
# define DM_MAGIC	0xc6000000L	/* dynamic mem */

# ifdef DEBUG
# define OFFSET		sizeof(header)
# else
# define OFFSET		sizeof(chunk)
# endif

typedef struct _chunk_ {
    long size;			/* size of chunk */
    struct _chunk_ *next;	/* next chunk */
} chunk;

# ifdef DEBUG
typedef struct _header_ {
    long size;			/* size of chunk */
    char *file;			/* file it was allocated from */
    int line;			/* line it was allocated from */
    struct _header_ *prev;	/* previous in list */
    struct _header_ *next;	/* next in list */
} header;
# endif


/*
 * NAME:	newmem()
 * DESCRIPTION:	allocate new memory
 */
static char *newmem(size)
unsigned int size;
{
    char *mem;

    mem = (char *) malloc(size);
    if (mem == (char *) NULL) {
	fatal("out of memory");
    }
    return mem;
}


/*
 * static memory manager
 */

# define INIT_MEM	16384		/* initial static memory chunk */
# define SLIMIT		(STRINGSZ + OFFSET)
# define SSMALL		(OFFSET + STRINGSZ / 8)
# define SCHUNKS	(STRINGSZ / STRUCT_AL - 1)
# define LCHUNKS	16

typedef struct {
    unsigned int size;		/* size of chunks in list */
    chunk *list;		/* list of chunks (possibly empty) */
} clist;

static chunk *schunk;			/* current chunk */
static unsigned int schunksz;		/* size of current chunk */
static chunk *schunks[SCHUNKS];		/* lists of small free chunks */
static clist lchunks[LCHUNKS];		/* lists of large free chunks */
static int nlc;				/* # elements in large chunk list */
static chunk *slist;			/* list of small unused chunks */
static int slevel;			/* static level */
static long smemsize;			/* static memory size */

# ifdef DEBUG
static header *hlist;			/* list of all dynamic memory chunks */
# endif

/*
 * NAME:	lchunk()
 * DESCRIPTION:	get the address of a list of large chunks
 */
static chunk **lchunk(size, new)
register unsigned int size;
bool new;
{
    register int h, l, m;

    l = m = 0;
    h = nlc;
    while (l < h) {
	m = (l + h) >> 1;
	if (lchunks[m].size == size) {
	    return &lchunks[m].list;	/* found */
	} else if (lchunks[m].size > size) {
	    h = m;			/* search in lower half */
	} else {
	    l = ++m;			/* search in upper half */
	}
    }

    if (!new) {
	return (chunk **) NULL;		/* not found */
    }
    /* make a new list */
    if (nlc == LCHUNKS) {
	fatal("too many different large static chunks");
    }
    for (l = nlc++; l > m; --l) {
	lchunks[l] = lchunks[l - 1];
    }
    lchunks[m].size = size;
    lchunks[m].list = (chunk *) NULL;
    return &lchunks[m].list;
}

/*
 * NAME:	salloc()
 * DESCRIPTION:	allocate static memory
 */
static chunk *salloc(size)
register unsigned int size;
{
    register chunk *c;

    /* try lists of free chunks */
    if (size >= SLIMIT) {
	register chunk **lc;

	lc = lchunk(size, FALSE);
	if (lc != (chunk **) NULL && *lc != (chunk *) NULL) {
	    /* large chunk */
	    c = *lc;
	    *lc = c->next;
	    return c;
	}
    } else if ((c=schunks[(size - OFFSET) / STRUCT_AL - 1]) != (chunk *) NULL) {
	/* small chunk */
	schunks[(size - OFFSET) / STRUCT_AL - 1] = c->next;
	return c;
    }

    /* try unused chunk list */
    if (slist != (chunk *) NULL && slist->size >= size) {
	if (slist->size - size <= OFFSET) {
	    /* remainder is too small to put in free list */
	    c = slist;
	    slist = c->next;
	} else {
	    register chunk *n;

	    /* split the chunk in two */
	    c = slist;
	    n = (chunk *) ((char *) slist + size);
	    n->size = c->size - size;
	    if (n->size <= SSMALL) {
		/* small chunk */
		n->next = schunks[(n->size - OFFSET) / STRUCT_AL - 1];
		schunks[(n->size - OFFSET) / STRUCT_AL - 1] = n;
		slist = c->next;
	    } else {
		/* large enough chunk */
		n->next = c->next;
		slist = n;
	    }
	    c->size = size;
	}
	return c;
    }

    /* try current chunk */
    if (schunk == (chunk *) NULL || (schunk->size < size && schunksz != 0)) {
	/*
	 * allocate default static memory block
	 */
	if (schunk != (chunk *) NULL) {
	    schunk->next = slist;
	    slist = schunk;
	}
	schunk = (chunk *) newmem(INIT_MEM);
	smemsize += schunk->size = INIT_MEM;
	if (schunksz != 0) {
	    /* memory has already been initialized */
	    fprintf(stderr,
		    "*** Ran out of static memory (increase static_chunk)\n");
	}
    }
    if (schunk->size >= size) {
	if (schunk->size - size <= OFFSET) {
	    /* remainder is too small */
	    c = schunk;
	    schunk = (chunk *) NULL;
	} else {
	    c = schunk;
	    schunk = (chunk *) ((char *) schunk + size);
	    if ((schunk->size=c->size - size) <= SSMALL) {
		/* small chunk */
		schunk->next = schunks[(schunk->size - OFFSET) / STRUCT_AL - 1];
		schunks[(schunk->size - OFFSET) / STRUCT_AL - 1] = schunk;
		schunk = (chunk *) NULL;
	    }
	    c->size = size;
	}
	return c;
    }

    /* allocate static memory directly */
    c = (chunk *) newmem(size);
    smemsize += c->size = size;
    return c;
}

/*
 * NAME:	sfree()
 * DESCRIPTION:	free static memory
 */
static void sfree(c)
register chunk *c;
{
    if (c->size < SLIMIT) {
	/* small chunk */
	c->next = schunks[(c->size - OFFSET) / STRUCT_AL - 1];
	schunks[(c->size - OFFSET) / STRUCT_AL - 1] = c;
    } else {
	register chunk **lc;

	/* large chunk */
	lc = lchunk((unsigned int) c->size, TRUE);
	c->next = *lc;
	*lc = c;
    }
}

/*
 * NAME:	mstatic()
 * DESCRIPTION:	enter static mode
 */
void mstatic()
{
    slevel++;
}

/*
 * NAME:	mdynamic()
 * DESCRIPTION:	reenter dynamic mode
 */
void mdynamic()
{
    --slevel;
}


/*
 * dynamic memory manager
 */

typedef struct _spnode_ {
    long size;			/* size of chunk */
    struct _spnode_ *parent;	/* parent node */
    struct _spnode_ *left;	/* left child node */
    struct _spnode_ *right;	/* right child node */
} spnode;

static unsigned int dchunksz;	/* dynamic chunk size */
static spnode *dtree;		/* splay tree of large dynamic free chunks */

/*
 * NAME:	insert()
 * DESCRIPTION:	insert a chunk in the splay tree
 */
static void insert(c)
chunk *c;
{
    register spnode *n, *t;
    register spnode *l, *r;
    register long size;

    n = dtree;
    dtree = t = (spnode *) c;
    t->parent = (spnode *) NULL;

    if (n == (spnode *) NULL) {
	/* first in splay tree */
	t->left = (spnode *) NULL;
	t->right = (spnode *) NULL;
    } else {
	size = t->size;
	l = r = t;

	for (;;) {
	    if (n->size < size) {
		if ((t=n->right) == (spnode *) NULL) {
		    l->right = n; n->parent = l;
		    r->left = (spnode *) NULL;
		    break;	/* finished */
		}
		if (t->size >= size) {
		    l->right = n; n->parent = l;
		    l = n;
		    n = t;
		    continue;
		}
		/* rotate */
		if ((n->right=t->left) != (spnode *) NULL) {
		    t->left->parent = n;
		}
		t->left = n; n->parent = t;
		l->right = t; t->parent = l;
		l = t;
		if ((n=t->right) == (spnode *) NULL) {
		    r->left = (spnode *) NULL;
		    break;	/* finished */
		}
	    } else {
		if ((t=n->left) == (spnode *) NULL) {
		    r->left = n; n->parent = r;
		    l->right = (spnode *) NULL;
		    break;	/* finished */
		}
		if (t->size < size) {
		    r->left = n; n->parent = r;
		    r = n;
		    n = t;
		    continue;
		}
		/* rotate */
		if ((n->left=t->right) != (spnode *) NULL) {
		    t->right->parent = n;
		}
		t->right = n; n->parent = t;
		r->left = t; t->parent = r;
		r = t;
		if ((n=t->left) == (spnode *) NULL) {
		    l->right = (spnode *) NULL;
		    break;	/* finished */
		}
	    }
	}

	/* exchange left and right subtree */
	n = dtree;
	t = n->left;
	n->left = n->right;
	n->right = t;
    }
}

/*
 * NAME:	seek()
 * DESCRIPTION:	find a chunk of the proper size in the splay tree
 */
static chunk *seek(size)
register long size;
{
    spnode dummy;
    register spnode *n, *t;
    register spnode *l, *r;

    if ((n=dtree) == (spnode *) NULL) {
	/* empty splay tree */
	return (chunk *) NULL;
    } else {
	l = r = &dummy;

	for (;;) {
	    if (n->size < size) {
		if ((t=n->right) == (spnode *) NULL) {
		    l->right = n; n->parent = l;
		    if (r == &dummy) {
			/* all chunks are too small */
			dtree = dummy.right;
			dtree->parent = (spnode *) NULL;
			return (chunk *) NULL;
		    }
		    if ((r->parent->left=r->right) != (spnode *) NULL) {
			r->right->parent = r->parent;
		    }
		    n = r;
		    break;	/* finished */
		}
		if (t->size >= size) {
		    l->right = n; n->parent = l;
		    l = n;
		    n = t;
		    continue;
		}
		/* rotate */
		if ((n->right=t->left) != (spnode *) NULL) {
		    t->left->parent = n;
		}
		t->left = n; n->parent = t;
		l->right = t; t->parent = l;
		l = t;
		if ((n=t->right) == (spnode *) NULL) {
		    if (r == &dummy) {
			/* all chunks are too small */
			dtree = dummy.right;
			dtree->parent = (spnode *) NULL;
			return (chunk *) NULL;
		    }
		    if ((r->parent->left=r->right) != (spnode *) NULL) {
			r->right->parent = r->parent;
		    }
		    n = r;
		    break;	/* finished */
		}
	    } else {
		if ((t=n->left) == (spnode *) NULL) {
		    if ((r->left=n->right) != (spnode *) NULL) {
			n->right->parent = r;
		    }
		    l->right = (spnode *) NULL;
		    break;	/* finished */
		}
		if (t->size < size) {
		    r->left = n; n->parent = r;
		    r = n;
		    n = t;
		    continue;
		}
		/* rotate */
		if ((n->left=t->right) != (spnode *) NULL) {
		    t->right->parent = n;
		}
		if (t->left == (spnode *) NULL) {
		    r->left = n; n->parent = r;
		    l->right = (spnode *) NULL;
		    n = t;
		    break;	/* finished */
		}
		t->right = n; n->parent = t;
		r->left = t; t->parent = r;
		r = t;
		n = t->left;
	    }
	}

	n->parent = (spnode *) NULL;
	if ((n->right=dummy.left) != (spnode *) NULL) {
	    dummy.left->parent = n;
	}
	if ((n->left=dummy.right) != (spnode *) NULL) {
	    dummy.right->parent = n;
	}

	return (chunk *) (dtree = n);
    }
}

/*
 * NAME:	delete()
 * DESCRIPTION:	delete a chunk from the splay tree
 */
static void delete(c)
chunk *c;
{
    register spnode *t, *r;
    register spnode *p, *n;

    n = (spnode *) c;
    p = n->parent;

    if (n->left == (spnode *) NULL) {
	/* there is no left subtree */
	if (p == (spnode *) NULL) {
	    if ((dtree=n->right) != (spnode *) NULL) {
		dtree->parent = (spnode *) NULL;
	    }
	} else if (n == p->left) {
	    if ((p->left=n->right) != (spnode *) NULL) {
		p->left->parent = p;
	    }
	} else if ((p->right=n->right) != (spnode *) NULL) {
	    p->right->parent = p;
	}
    } else {
	t = n->left;

	/* walk to the right in the left subtree */
	while ((r=t->right) != (spnode *) NULL) {
	    if ((t->right=r->left) != (spnode *) NULL) {
		r->left->parent = t;
	    }
	    r->left = t; t->parent = r;
	    t = r;
	}

	if (p == (spnode *) NULL) {
	    dtree = t;
	} else if (n == p->left) {
	    p->left = t;
	} else {
	    p->right = t;
	}
	t->parent = p;
	if ((t->right=n->right) != (spnode *) NULL) {
	    t->right->parent = t;
	}
    }
}

# define DSMALL		48
# define DLIMIT		(DSMALL + OFFSET)
# define DCHUNKS	(DSMALL / STRUCT_AL - 1)
# define DCHUNKSZ	16384

static char *dlist;		/* list of dynamic memory chunks */
static chunk *dchunks[DCHUNKS];	/* list of free small chunks */
static chunk *dchunk;		/* chunk of small chunks */
static long dmemsize;		/* dynamic memory allocated */
static long dmemused;		/* dynamic memory used */

/*
 * NAME:	dalloc()
 * DESCRIPTION:	allocate dynamic memory
 */
static chunk *dalloc(size)
register unsigned int size;
{
    register chunk *c;
    register char *p;
    register unsigned int sz;

    if (dchunksz == 0) {
	/*
	 * memory manager hasn't been initialized yet
	 */
	c = (chunk *) newmem(size);
	c->size = size;
	return c;
    }

    if (size < DLIMIT) {
	/*
	 * small chunk
	 */
	if ((c=dchunks[(size - OFFSET) / STRUCT_AL - 1]) != (chunk *) NULL) {
	    /* small chunk from free list */
	    dchunks[(size - OFFSET) / STRUCT_AL - 1] = c->next;
	    return c;
	}
	if (dchunk == (chunk *) NULL) {
	    /* get new chunks chunk */
	    dchunk = dalloc(DCHUNKSZ);	/* cannot use alloc() here */
	    p = (char *) dchunk + ALIGN(sizeof(long), STRUCT_AL);
	    ((chunk *) p)->size = dchunk->size -
				  ALIGN(sizeof(long), STRUCT_AL) - sizeof(long);
	    dchunk->size |= DM_MAGIC;
	    dchunk = (chunk *) p;
	}
	sz = dchunk->size - size;
	c = dchunk;
	c->size = size;
	if (sz >= DLIMIT - STRUCT_AL) {
	    /* enough is left for another small chunk */
	    dchunk = (chunk *) ((char *) c + size);
	    dchunk->size = sz;
	} else {
	    dchunk = (chunk *) NULL;
	}
	return c;
    }

    size += sizeof(long);
    c = seek((long) size);
    if (c != (chunk *) NULL) {
	/*
	 * remove from free list
	 */
	delete(c);
    } else {
	/*
	 * get new dynamic chunk
	 */
	p = newmem(dchunksz);
	dmemsize += dchunksz;
	*(char **) p = dlist;
	dlist = p;
	p += sizeof(char *);

	/* no previous chunk */
	*(long *) p = 0;
	c = (chunk *) (p + sizeof(long));
	/* initialize chunk */
	c->size = dchunksz - 2 * sizeof(long) - sizeof(char *);
	p += c->size;
	*(long *) p = c->size;
	/* no following chunk */
	p += sizeof(long);
	((chunk *) p)->size = 0;

	if (c->size < size) {
	    fatal("too small dynamic_chunk");
	}
    }

    if ((sz=c->size - size) >= DLIMIT + sizeof(long)) {
	/*
	 * split block, put second part in free list
	 */
	c->size = size;
	p = (char *) c + size - sizeof(long);
	*(long *) p = size;
	p += sizeof(long);
	((chunk *) p)->size = sz;
	*((long *) (p + sz - sizeof(long))) = sz;
	insert((chunk *) p);	/* add to free list */
    }
    return c;
}

/*
 * NAME:	dfree()
 * DESCRIPTION:	free dynamic memory
 */
static void dfree(c)
register chunk *c;
{
    register char *p;

    if (dchunksz == 0) {
	/*
	 * memory manager not yet initialized
	 */
	free((char *) c);
	return;
    }

    if (c->size < DLIMIT) {
	/* small chunk */
	c->next = dchunks[(c->size - OFFSET) / STRUCT_AL - 1];
	dchunks[(c->size - OFFSET) / STRUCT_AL - 1] = c;
	return;
    }

    p = (char *) c - sizeof(long);
    if (*(long *) p != 0) {
	p -= *(long *) p - sizeof(long);
	if ((((chunk *) p)->size & MAGIC_MASK) == 0) {
	    /*
	     * merge with previous block
	     */
	    delete((chunk *) p);
	    ((chunk *) p)->size += c->size;
	    c = (chunk *) p;
	    *((long *) (p + c->size - sizeof(long))) = c->size;
	}
    }
    p = (char*) c + c->size;
    if (*(long *) p != 0 && (((chunk *) p)->size & MAGIC_MASK) == 0) {
	/*
	 * merge with next block
	 */
	delete((chunk *) p);
	c->size += ((chunk *) p)->size;
	*((long *) ((char *) c + c->size - sizeof(long))) = c->size;
    }

    insert(c);	/* add to free list */
}


/*
 * NAME:	minit()
 * DESCRIPTION:	initialize memory manager
 */
void minit(ssz, dsz)
unsigned int ssz, dsz;
{
    schunksz = ssz;
    dchunksz = dsz;
    if (schunk != (chunk *) NULL) {
	schunk->next = slist;
	slist = schunk;
    }
    schunk = (chunk *) newmem(ssz);
    smemsize += schunk->size = ssz;
}


/*
 * NAME:	alloc()
 * DESCRIPTION:	allocate memory
 */
# ifdef DEBUG
char *alloc(size, file, line)
register unsigned int size;
char *file;
int line;
# else
char *alloc(size)
register unsigned int size;
# endif
{
    register chunk *c;

# ifdef DEBUG
    if (size == 0) {
	fatal("alloc(0)");
    }
# endif
    size = ALIGN(size + OFFSET, STRUCT_AL);
# ifndef DEBUG
    if (size < sizeof(chunk)) {
	size = sizeof(chunk);
    }
# endif
    if (slevel > 0) {
	c = salloc(size);
	c->size |= SM_MAGIC;
    } else {
	c = dalloc(size);
	dmemused += c->size;
	c->size |= DM_MAGIC;
# ifdef DEBUG
	((header *) c)->prev = (header *) NULL;
	((header *) c)->next = hlist;
	if (hlist != (header *) NULL) {
	    hlist->prev = (header *) c;
	}
	hlist = (header *) c;
# endif
    }
# ifdef DEBUG
    ((header *) c)->file = file;
    ((header *) c)->line = line;
    return (char *) c + sizeof(header);
# else
    return (char *) c + sizeof(long);
# endif
}

/*
 * NAME:	mfree()
 * DESCRIPTION:	free memory
 */
void mfree(mem)
char *mem;
{
    register chunk *c;

# ifdef DEBUG
    c = (chunk *) (mem - sizeof(header));
# else
    c = (chunk *) (mem - sizeof(long));
# endif
    if ((c->size & MAGIC_MASK) == SM_MAGIC) {
	c->size &= SIZE_MASK;
	sfree(c);
    } else if ((c->size & MAGIC_MASK) == DM_MAGIC) {
	c->size &= SIZE_MASK;
	dmemused -= c->size;
# ifdef DEBUG
	if (((header *) c)->next != (header *) NULL) {
	    ((header *) c)->next->prev = ((header *) c)->prev;
	}
	if (((header *) c) == hlist) {
	    hlist = ((header *) c)->next;
	} else {
	    ((header *) c)->prev->next = ((header *) c)->next;
	}
# endif
	dfree(c);
    } else {
	fatal("bad pointer in mfree");
    }
}

/*
 * NAME:	mcheck()
 * DESCRIPTION:	return TRUE if there is enough static memory left, or FALSE
 *		otherwise
 */
bool mcheck()
{
    if (schunk == (chunk *) NULL) {
	return FALSE;
    } else {
	return (schunk->size >= schunksz);
    }
}

/*
 * NAME:	mpurge()
 * DESCRIPTION:	purge dynamic memory
 */
void mpurge()
{
    register char *p;

# ifdef DEBUG
    while (hlist != (header *) NULL) {
	register long n;
	register char *mem;

	n = (hlist->size & SIZE_MASK) - sizeof(header);
	if (n >= DLIMIT) {
	    n -= sizeof(long);
	}
	fprintf(stderr, "FREE(%08X/%ld), %s line %d:\n", hlist + 1, n,
		hlist->file, hlist->line);
	if (n > 26) {
	    n = 26;
	}
	for (mem = (char *) (hlist + 1); n > 0; --n, mem++) {
	    if (*mem >= ' ') {
		fprintf(stderr, " '%c", *mem);
	    } else {
		fprintf(stderr, " %02x", UCHAR(*mem));
	    }
	}
	fputc('\n', stderr);
	fflush(stderr);
	mfree((char *) (hlist + 1));
    }
# endif

    memset(dchunks, '\0', sizeof(dchunks));
    dchunk = (chunk *) NULL;
    dtree = (spnode *) NULL;
    while (dlist != (char *) NULL) {
	p = dlist;
	dlist = *(char **) p;
	free(p);
    }
    dmemsize = dmemused = 0;
}

/*
 * NAME:	mexpand()
 * DESCRIPTION:	expand the static memory area
 */
void mexpand()
{
    if (schunk != (chunk *) NULL) {
	schunk->next = slist;
	slist = schunk;
    }
    schunk = (chunk *) newmem(schunksz);
    smemsize += schunk->size = schunksz;
}

/*
 * NAME:	memsize()
 * DESCRIPTION:	return the amount of memory allocated
 */
long memsize()
{
    return smemsize + dmemsize;
}

/*
 * NAME:	memused()
 * DESCRIPTION:	return the amount of memory used
 */
long memused()
{
    return smemsize + dmemused;
}
