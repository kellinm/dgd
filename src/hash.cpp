/*
 * This file is part of DGD, https://github.com/dworkin/dgd
 * Copyright (C) 1993-2010 Dworkin B.V.
 * Copyright (C) 2010-2016 DGD Authors (see the commit log for details)
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
# include "hash.h"

/*
 * Generic string hash table.
 */

unsigned char Hashtab::tab[256] = {
    0001, 0127, 0061, 0014, 0260, 0262, 0146, 0246,
    0171, 0301, 0006, 0124, 0371, 0346, 0054, 0243,
    0016, 0305, 0325, 0265, 0241, 0125, 0332, 0120,
    0100, 0357, 0030, 0342, 0354, 0216, 0046, 0310,
    0156, 0261, 0150, 0147, 0215, 0375, 0377, 0062,
    0115, 0145, 0121, 0022, 0055, 0140, 0037, 0336,
    0031, 0153, 0276, 0106, 0126, 0355, 0360, 0042,
    0110, 0362, 0024, 0326, 0364, 0343, 0225, 0353,
    0141, 0352, 0071, 0026, 0074, 0372, 0122, 0257,
    0320, 0005, 0177, 0307, 0157, 0076, 0207, 0370,
    0256, 0251, 0323, 0072, 0102, 0232, 0152, 0303,
    0365, 0253, 0021, 0273, 0266, 0263, 0000, 0363,
    0204, 0070, 0224, 0113, 0200, 0205, 0236, 0144,
    0202, 0176, 0133, 0015, 0231, 0366, 0330, 0333,
    0167, 0104, 0337, 0116, 0123, 0130, 0311, 0143,
    0172, 0013, 0134, 0040, 0210, 0162, 0064, 0012,
    0212, 0036, 0060, 0267, 0234, 0043, 0075, 0032,
    0217, 0112, 0373, 0136, 0201, 0242, 0077, 0230,
    0252, 0007, 0163, 0247, 0361, 0316, 0003, 0226,
    0067, 0073, 0227, 0334, 0132, 0065, 0027, 0203,
    0175, 0255, 0017, 0356, 0117, 0137, 0131, 0020,
    0151, 0211, 0341, 0340, 0331, 0240, 0045, 0173,
    0166, 0111, 0002, 0235, 0056, 0164, 0011, 0221,
    0206, 0344, 0317, 0324, 0312, 0327, 0105, 0345,
    0033, 0274, 0103, 0174, 0250, 0374, 0052, 0004,
    0035, 0154, 0025, 0367, 0023, 0315, 0047, 0313,
    0351, 0050, 0272, 0223, 0306, 0300, 0233, 0041,
    0244, 0277, 0142, 0314, 0245, 0264, 0165, 0114,
    0214, 0044, 0322, 0254, 0051, 0066, 0237, 0010,
    0271, 0350, 0161, 0304, 0347, 0057, 0222, 0170,
    0063, 0101, 0034, 0220, 0376, 0335, 0135, 0275,
    0302, 0213, 0160, 0053, 0107, 0155, 0270, 0321,
};

/*
 * NAME:	Hashtab::create()
 * DESCRIPTION:	hashtable factory
 */
Hashtab *Hashtab::create(unsigned int size, unsigned int maxlen, bool mem)
{
    return new HashtabImpl(size, maxlen, mem);
}

/*
 * NAME:	Hashtab::hashstr()
 * DESCRIPTION:	Hash string s, considering at most len characters. Return
 *		an unsigned modulo size.
 *		Based on Peter K. Pearson's article in CACM 33-6, pp 677.
 */
unsigned short Hashtab::hashstr(const char *str, unsigned int len)
{
    unsigned char h, l;

    h = l = 0;
    while (*str != '\0' && len > 0) {
	h = l;
	l = tab[l ^ (unsigned char) *str++];
	--len;
    }
    return (unsigned short) ((h << 8) | l);
}

/*
 * NAME:	Hashtab::hashmem()
 * DESCRIPTION:	hash memory
 */
unsigned short Hashtab::hashmem(const char *mem, unsigned int len)
{
    unsigned char h, l;

    h = l = 0;
    while (len > 0) {
	h = l;
	l = tab[l ^ (unsigned char) *mem++];
	--len;
    }
    return (unsigned short) ((h << 8) | l);
}


/*
 * NAME:	HashtabImpl()
 * DESCRIPTION:	create a new hashtable of size "size", where "maxlen" characters
 *		of each string are significant
 */
HashtabImpl::HashtabImpl(unsigned int size, unsigned int maxlen, bool mem)
{
    m_size = size;
    m_maxlen = maxlen;
    m_mem = mem;
    m_table = ALLOC(Entry*, size);
    memset(m_table, '\0', size * sizeof(Entry*));
}

/*
 * NAME:	~HashtabImpl()
 * DESCRIPTION:	delete a hash table
 */
HashtabImpl::~HashtabImpl()
{
    FREE(m_table);
}

/*
 * NAME:	HashtabImpl::lookup()
 * DESCRIPTION:	lookup a name in a hashtable, return the address of the entry
 *		or &NULL if none found
 */
Hashtab::Entry **HashtabImpl::lookup(const char *name, bool move)
{
    Entry **first, **e, *next;

    if (m_mem) {
	first = e = &(m_table[hashmem(name, m_maxlen) % m_size]);
	while (*e != (Entry *) NULL) {
	    if (memcmp((*e)->name, name, m_maxlen) == 0) {
		if (move && e != first) {
		    /* move to first position */
		    next = (*e)->next;
		    (*e)->next = *first;
		    *first = *e;
		    *e = next;
		    return first;
		}
		break;
	    }
	    e = &((*e)->next);
	}
    } else {
	first = e = &(m_table[hashstr(name, m_maxlen) % m_size]);
	while (*e != (Entry *) NULL) {
	    if (strcmp((*e)->name, name) == 0) {
		if (move && e != first) {
		    /* move to first position */
		    next = (*e)->next;
		    (*e)->next = *first;
		    *first = *e;
		    *e = next;
		    return first;
		}
		break;
	    }
	    e = &((*e)->next);
	}
    }
    return e;
}
