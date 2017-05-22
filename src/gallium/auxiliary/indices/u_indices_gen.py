copyright = '''
/*
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * VMWARE AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
'''

GENERATE, UBYTE, USHORT, UINT = 'generate', 'ubyte', 'ushort', 'uint'
FIRST, LAST = 'first', 'last'
PRDISABLE, PRENABLE = 'prdisable', 'prenable'

INTYPES = (GENERATE, UBYTE, USHORT, UINT)
OUTTYPES = (USHORT, UINT)
PVS=(FIRST, LAST)
PRS=(PRDISABLE, PRENABLE)
PRIMS=('points',
       'lines',
       'linestrip',
       'lineloop',
       'tris',
       'trifan',
       'tristrip',
       'quads',
       'quadstrip',
       'polygon',
       'linesadj',
       'linestripadj',
       'trisadj',
       'tristripadj')

LONGPRIMS=('PIPE_PRIM_POINTS',
           'PIPE_PRIM_LINES',
           'PIPE_PRIM_LINE_STRIP',
           'PIPE_PRIM_LINE_LOOP',
           'PIPE_PRIM_TRIANGLES',
           'PIPE_PRIM_TRIANGLE_FAN',
           'PIPE_PRIM_TRIANGLE_STRIP',
           'PIPE_PRIM_QUADS',
           'PIPE_PRIM_QUAD_STRIP',
           'PIPE_PRIM_POLYGON',
           'PIPE_PRIM_LINES_ADJACENCY',
           'PIPE_PRIM_LINE_STRIP_ADJACENCY',
           'PIPE_PRIM_TRIANGLES_ADJACENCY',
           'PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY')

longprim = dict(zip(PRIMS, LONGPRIMS))
intype_idx = dict(ubyte='IN_UBYTE', ushort='IN_USHORT', uint='IN_UINT')
outtype_idx = dict(ushort='OUT_USHORT', uint='OUT_UINT')
pv_idx = dict(first='PV_FIRST', last='PV_LAST')
pr_idx = dict(prdisable='PR_DISABLE', prenable='PR_ENABLE')

def prolog():
    print '''/* File automatically generated by u_indices_gen.py */'''
    print copyright
    print r'''

/**
 * @file
 * Functions to translate and generate index lists
 */

#include "indices/u_indices_priv.h"
#include "util/u_debug.h"
#include "util/u_memory.h"


static unsigned out_size_idx( unsigned index_size )
{
   switch (index_size) {
   case 4: return OUT_UINT;
   case 2: return OUT_USHORT;
   default: assert(0); return OUT_USHORT;
   }
}

static unsigned in_size_idx( unsigned index_size )
{
   switch (index_size) {
   case 4: return IN_UINT;
   case 2: return IN_USHORT;
   case 1: return IN_UBYTE;
   default: assert(0); return IN_UBYTE;
   }
}


static u_translate_func translate[IN_COUNT][OUT_COUNT][PV_COUNT][PV_COUNT][PR_COUNT][PRIM_COUNT];
static u_generate_func  generate[OUT_COUNT][PV_COUNT][PV_COUNT][PRIM_COUNT];


'''

def vert( intype, outtype, v0 ):
    if intype == GENERATE:
        return '(' + outtype + ')(' + v0 + ')'
    else:
        return '(' + outtype + ')in[' + v0 + ']'

def point( intype, outtype, ptr, v0 ):
    print '      (' + ptr + ')[0] = ' + vert( intype, outtype, v0 ) + ';'

def line( intype, outtype, ptr, v0, v1 ):
    print '      (' + ptr + ')[0] = ' + vert( intype, outtype, v0 ) + ';'
    print '      (' + ptr + ')[1] = ' + vert( intype, outtype, v1 ) + ';'

def tri( intype, outtype, ptr, v0, v1, v2 ):
    print '      (' + ptr + ')[0] = ' + vert( intype, outtype, v0 ) + ';'
    print '      (' + ptr + ')[1] = ' + vert( intype, outtype, v1 ) + ';'
    print '      (' + ptr + ')[2] = ' + vert( intype, outtype, v2 ) + ';'

def lineadj( intype, outtype, ptr, v0, v1, v2, v3 ):
    print '      (' + ptr + ')[0] = ' + vert( intype, outtype, v0 ) + ';'
    print '      (' + ptr + ')[1] = ' + vert( intype, outtype, v1 ) + ';'
    print '      (' + ptr + ')[2] = ' + vert( intype, outtype, v2 ) + ';'
    print '      (' + ptr + ')[3] = ' + vert( intype, outtype, v3 ) + ';'

def triadj( intype, outtype, ptr, v0, v1, v2, v3, v4, v5 ):
    print '      (' + ptr + ')[0] = ' + vert( intype, outtype, v0 ) + ';'
    print '      (' + ptr + ')[1] = ' + vert( intype, outtype, v1 ) + ';'
    print '      (' + ptr + ')[2] = ' + vert( intype, outtype, v2 ) + ';'
    print '      (' + ptr + ')[3] = ' + vert( intype, outtype, v3 ) + ';'
    print '      (' + ptr + ')[4] = ' + vert( intype, outtype, v4 ) + ';'
    print '      (' + ptr + ')[5] = ' + vert( intype, outtype, v5 ) + ';'

def do_point( intype, outtype, ptr, v0 ):
    point( intype, outtype, ptr, v0 )

def do_line( intype, outtype, ptr, v0, v1, inpv, outpv ):
    if inpv == outpv:
        line( intype, outtype, ptr, v0, v1 )
    else:
        line( intype, outtype, ptr, v1, v0 )

def do_tri( intype, outtype, ptr, v0, v1, v2, inpv, outpv ):
    if inpv == outpv:
        tri( intype, outtype, ptr, v0, v1, v2 )
    else:
        if inpv == FIRST:
            tri( intype, outtype, ptr, v1, v2, v0 )
        else:
            tri( intype, outtype, ptr, v2, v0, v1 )

def do_quad( intype, outtype, ptr, v0, v1, v2, v3, inpv, outpv ):
    if inpv == LAST:
        do_tri( intype, outtype, ptr+'+0',  v0, v1, v3, inpv, outpv );
        do_tri( intype, outtype, ptr+'+3',  v1, v2, v3, inpv, outpv );
    else:
        do_tri( intype, outtype, ptr+'+0',  v0, v1, v2, inpv, outpv );
        do_tri( intype, outtype, ptr+'+3',  v0, v2, v3, inpv, outpv );

def do_lineadj( intype, outtype, ptr, v0, v1, v2, v3, inpv, outpv ):
    if inpv == outpv:
        lineadj( intype, outtype, ptr, v0, v1, v2, v3 )
    else:
        lineadj( intype, outtype, ptr, v3, v2, v1, v0 )

def do_triadj( intype, outtype, ptr, v0, v1, v2, v3, v4, v5, inpv, outpv ):
    if inpv == outpv:
        triadj( intype, outtype, ptr, v0, v1, v2, v3, v4, v5 )
    else:
        triadj( intype, outtype, ptr, v4, v5, v0, v1, v2, v3 )

def name(intype, outtype, inpv, outpv, pr, prim):
    if intype == GENERATE:
        return 'generate_' + prim + '_' + outtype + '_' + inpv + '2' + outpv
    else:
        return 'translate_' + prim + '_' + intype + '2' + outtype + '_' + inpv + '2' + outpv + '_' + pr

def preamble(intype, outtype, inpv, outpv, pr, prim):
    print 'static void ' + name( intype, outtype, inpv, outpv, pr, prim ) + '('
    if intype != GENERATE:
        print '    const void * _in,'
    print '    unsigned start,'
    if intype != GENERATE:
        print '    unsigned in_nr,'
    print '    unsigned out_nr,'
    if intype != GENERATE:
        print '    unsigned restart_index,'
    print '    void *_out )'
    print '{'
    if intype != GENERATE:
        print '  const ' + intype + '*in = (const ' + intype + '*)_in;'
    print '  ' + outtype + ' *out = (' + outtype + '*)_out;'
    print '  unsigned i, j;'
    print '  (void)j;'

def postamble():
    print '}'


def points(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='points')
    print '  for (i = start; i < (out_nr+start); i++) { '
    do_point( intype, outtype, 'out+i',  'i' );
    print '   }'
    postamble()

def lines(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='lines')
    print '  for (i = start; i < (out_nr+start); i+=2) { '
    do_line( intype, outtype, 'out+i',  'i', 'i+1', inpv, outpv );
    print '   }'
    postamble()

def linestrip(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='linestrip')
    print '  for (i = start, j = 0; j < out_nr; j+=2, i++) { '
    do_line( intype, outtype, 'out+j',  'i', 'i+1', inpv, outpv );
    print '   }'
    postamble()

def lineloop(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='lineloop')
    print '  for (i = start, j = 0; j < out_nr - 2; j+=2, i++) { '
    do_line( intype, outtype, 'out+j',  'i', 'i+1', inpv, outpv );
    print '   }'
    do_line( intype, outtype, 'out+j',  'i', 'start', inpv, outpv );
    postamble()

def tris(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='tris')
    print '  for (i = start; i < (out_nr+start); i+=3) { '
    do_tri( intype, outtype, 'out+i',  'i', 'i+1', 'i+2', inpv, outpv );
    print '   }'
    postamble()


def tristrip(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='tristrip')
    print '  for (i = start, j = 0; j < out_nr; j+=3, i++) { '
    if inpv == FIRST:
        do_tri( intype, outtype, 'out+j',  'i', 'i+1+(i&1)', 'i+2-(i&1)', inpv, outpv );
    else:
        do_tri( intype, outtype, 'out+j',  'i+(i&1)', 'i+1-(i&1)', 'i+2', inpv, outpv );
    print '   }'
    postamble()


def trifan(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='trifan')
    print '  for (i = start, j = 0; j < out_nr; j+=3, i++) { '
    do_tri( intype, outtype, 'out+j',  'start', 'i+1', 'i+2', inpv, outpv );
    print '   }'
    postamble()



def polygon(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='polygon')
    print '  for (i = start, j = 0; j < out_nr; j+=3, i++) { '
    if pr == PRENABLE:
        print 'restart:'
        print '      if (i + 3 > in_nr) {'
        print '         (out+j+0)[0] = restart_index;'
        print '         (out+j+0)[1] = restart_index;'
        print '         (out+j+0)[2] = restart_index;'
        print '         continue;'
        print '      }'
        print '      if (in[i + 0] == restart_index) {'
        print '         i += 1;'
        print '         start = i;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 1] == restart_index) {'
        print '         i += 2;'
        print '         start = i;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 2] == restart_index) {'
        print '         i += 3;'
        print '         start = i;'
        print '         goto restart;'
        print '      }'

    if inpv == FIRST:
        do_tri( intype, outtype, 'out+j',  'start', 'i+1', 'i+2', inpv, outpv );
    else:
        do_tri( intype, outtype, 'out+j',  'i+1', 'i+2', 'start', inpv, outpv );
    print '   }'
    postamble()


def quads(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='quads')
    print '  for (i = start, j = 0; j < out_nr; j+=6, i+=4) { '
    if pr == PRENABLE:
        print 'restart:'
        print '      if (i + 4 > in_nr) {'
        print '         (out+j+0)[0] = restart_index;'
        print '         (out+j+0)[1] = restart_index;'
        print '         (out+j+0)[2] = restart_index;'
        print '         (out+j+3)[0] = restart_index;'
        print '         (out+j+3)[1] = restart_index;'
        print '         (out+j+3)[2] = restart_index;'
        print '         continue;'
        print '      }'
        print '      if (in[i + 0] == restart_index) {'
        print '         i += 1;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 1] == restart_index) {'
        print '         i += 2;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 2] == restart_index) {'
        print '         i += 3;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 3] == restart_index) {'
        print '         i += 4;'
        print '         goto restart;'
        print '      }'

    do_quad( intype, outtype, 'out+j', 'i+0', 'i+1', 'i+2', 'i+3', inpv, outpv );
    print '   }'
    postamble()


def quadstrip(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='quadstrip')
    print '  for (i = start, j = 0; j < out_nr; j+=6, i+=2) { '
    if pr == PRENABLE:
        print 'restart:'
        print '      if (i + 4 > in_nr) {'
        print '         (out+j+0)[0] = restart_index;'
        print '         (out+j+0)[1] = restart_index;'
        print '         (out+j+0)[2] = restart_index;'
        print '         (out+j+3)[0] = restart_index;'
        print '         (out+j+3)[1] = restart_index;'
        print '         (out+j+3)[2] = restart_index;'
        print '         continue;'
        print '      }'
        print '      if (in[i + 0] == restart_index) {'
        print '         i += 1;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 1] == restart_index) {'
        print '         i += 2;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 2] == restart_index) {'
        print '         i += 3;'
        print '         goto restart;'
        print '      }'
        print '      if (in[i + 3] == restart_index) {'
        print '         i += 4;'
        print '         goto restart;'
        print '      }'
    if inpv == LAST:
        do_quad( intype, outtype, 'out+j', 'i+2', 'i+0', 'i+1', 'i+3', inpv, outpv );
    else:
        do_quad( intype, outtype, 'out+j', 'i+0', 'i+1', 'i+3', 'i+2', inpv, outpv );
    print '   }'
    postamble()


def linesadj(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='linesadj')
    print '  for (i = start; i < (out_nr+start); i+=4) { '
    do_lineadj( intype, outtype, 'out+i',  'i+0', 'i+1', 'i+2', 'i+3', inpv, outpv )
    print '  }'
    postamble()


def linestripadj(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='linestripadj')
    print '  for (i = start, j = 0; j < out_nr; j+=4, i++) {'
    do_lineadj( intype, outtype, 'out+j',  'i+0', 'i+1', 'i+2', 'i+3', inpv, outpv )
    print '  }'
    postamble()


def trisadj(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='trisadj')
    print '  for (i = start; i < (out_nr+start); i+=6) { '
    do_triadj( intype, outtype, 'out+i',  'i+0', 'i+1', 'i+2', 'i+3',
               'i+4', 'i+5', inpv, outpv )
    print '  }'
    postamble()


def tristripadj(intype, outtype, inpv, outpv, pr):
    preamble(intype, outtype, inpv, outpv, pr, prim='tristripadj')
    print '  for (i = start, j = 0; j < out_nr; i+=2, j+=6) { '
    print '    if (i % 4 == 0) {'
    print '      /* even triangle */'
    do_triadj( intype, outtype, 'out+j',
               'i+0', 'i+1', 'i+2', 'i+3', 'i+4', 'i+5', inpv, outpv )
    print '    } else {'
    print '      /* odd triangle */'
    do_triadj( intype, outtype, 'out+j',
               'i+2', 'i-2', 'i+0', 'i+3', 'i+4', 'i+6', inpv, outpv )
    print '    }'
    print '  }'
    postamble()


def emit_funcs():
    for intype in INTYPES:
        for outtype in OUTTYPES:
            for inpv in (FIRST, LAST):
                for outpv in (FIRST, LAST):
                    for pr in (PRDISABLE, PRENABLE):
                        if pr == PRENABLE and intype == GENERATE:
                            continue
                        points(intype, outtype, inpv, outpv, pr)
                        lines(intype, outtype, inpv, outpv, pr)
                        linestrip(intype, outtype, inpv, outpv, pr)
                        lineloop(intype, outtype, inpv, outpv, pr)
                        tris(intype, outtype, inpv, outpv, pr)
                        tristrip(intype, outtype, inpv, outpv, pr)
                        trifan(intype, outtype, inpv, outpv, pr)
                        quads(intype, outtype, inpv, outpv, pr)
                        quadstrip(intype, outtype, inpv, outpv, pr)
                        polygon(intype, outtype, inpv, outpv, pr)
                        linesadj(intype, outtype, inpv, outpv, pr)
                        linestripadj(intype, outtype, inpv, outpv, pr)
                        trisadj(intype, outtype, inpv, outpv, pr)
                        tristripadj(intype, outtype, inpv, outpv, pr)

def init(intype, outtype, inpv, outpv, pr, prim):
    if intype == GENERATE:
        print ('generate[' +
               outtype_idx[outtype] +
               '][' + pv_idx[inpv] +
               '][' + pv_idx[outpv] +
               '][' + longprim[prim] +
               '] = ' + name( intype, outtype, inpv, outpv, pr, prim ) + ';')
    else:
        print ('translate[' +
               intype_idx[intype] +
               '][' + outtype_idx[outtype] +
               '][' + pv_idx[inpv] +
               '][' + pv_idx[outpv] +
               '][' + pr_idx[pr] +
               '][' + longprim[prim] +
               '] = ' + name( intype, outtype, inpv, outpv, pr, prim ) + ';')


def emit_all_inits():
    for intype in INTYPES:
        for outtype in OUTTYPES:
            for inpv in PVS:
                for outpv in PVS:
                    for pr in PRS:
                        for prim in PRIMS:
                            init(intype, outtype, inpv, outpv, pr, prim)

def emit_init():
    print 'void u_index_init( void )'
    print '{'
    print '  static int firsttime = 1;'
    print '  if (!firsttime) return;'
    print '  firsttime = 0;'
    emit_all_inits()
    print '}'




def epilog():
    print '#include "indices/u_indices.c"'


def main():
    prolog()
    emit_funcs()
    emit_init()
    epilog()


if __name__ == '__main__':
    main()
