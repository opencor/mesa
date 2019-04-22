OPCODE(1, 1, COMP, ARL)
OPCODE(1, 1, COMP, MOV)
OPCODE(1, 1, CHAN, LIT)
OPCODE(1, 1, REPL, RCP)
OPCODE(1, 1, REPL, RSQ)
OPCODE(1, 1, CHAN, EXP)
OPCODE(1, 1, CHAN, LOG)
OPCODE(1, 2, COMP, MUL)
OPCODE(1, 2, COMP, ADD)
OPCODE(1, 2, REPL, DP3)
OPCODE(1, 2, REPL, DP4)
OPCODE(1, 2, CHAN, DST)
OPCODE(1, 2, COMP, MIN)
OPCODE(1, 2, COMP, MAX)
OPCODE(1, 2, COMP, SLT)
OPCODE(1, 2, COMP, SGE)
OPCODE(1, 3, COMP, MAD)
OPCODE(1, 2, OTHR, TEX_LZ, .is_tex = 1)
OPCODE(1, 3, COMP, LRP)
OPCODE(1, 3, COMP, FMA)
OPCODE(1, 1, REPL, SQRT)
OPCODE(1, 2, COMP, LDEXP)
OPCODE(1, 1, COMP, F2U64)
OPCODE(1, 1, COMP, F2I64)
OPCODE(1, 1, COMP, FRC)
OPCODE(1, 2, OTHR, TXF_LZ, .is_tex = 1)
OPCODE(1, 1, COMP, FLR)
OPCODE(1, 1, COMP, ROUND)
OPCODE(1, 1, REPL, EX2)
OPCODE(1, 1, REPL, LG2)
OPCODE(1, 2, REPL, POW)
OPCODE_GAP(31) /* removed */
OPCODE(1, 1, COMP, U2I64)
OPCODE(1, 0, OTHR, CLOCK)
OPCODE(1, 1, COMP, I2I64)
OPCODE_GAP(35) /* removed */
OPCODE(1, 1, REPL, COS)
OPCODE(1, 1, COMP, DDX)
OPCODE(1, 1, COMP, DDY)
OPCODE(0, 0, NONE, KILL)
OPCODE(1, 1, REPL, PK2H)
OPCODE(1, 1, REPL, PK2US)
OPCODE(1, 1, REPL, PK4B)
OPCODE(1, 1, REPL, PK4UB)
OPCODE(1, 1, COMP, D2U64)
OPCODE(1, 2, COMP, SEQ)
OPCODE(1, 1, COMP, D2I64)
OPCODE(1, 2, COMP, SGT)
OPCODE(1, 1, REPL, SIN)
OPCODE(1, 2, COMP, SLE)
OPCODE(1, 2, COMP, SNE)
OPCODE(1, 1, COMP, U642D)
OPCODE(1, 2, OTHR, TEX, .is_tex = 1)
OPCODE(1, 4, OTHR, TXD, .is_tex = 1)
OPCODE(1, 2, OTHR, TXP, .is_tex = 1)
OPCODE(1, 1, CHAN, UP2H)
OPCODE(1, 1, CHAN, UP2US)
OPCODE(1, 1, CHAN, UP4B)
OPCODE(1, 1, CHAN, UP4UB)
OPCODE(1, 1, COMP, U642F)
OPCODE(1, 1, COMP, I642F)
OPCODE(1, 1, COMP, ARR)
OPCODE(1, 1, COMP, I642D)
OPCODE(0, 0, NONE, CAL, .is_branch = 1)
OPCODE(0, 0, NONE, RET)
OPCODE(1, 1, COMP, SSG)
OPCODE(1, 3, COMP, CMP)
OPCODE_GAP(67) /* removed */
OPCODE(1, 2, OTHR, TXB, .is_tex = 1)
OPCODE(1, 1, OTHR, FBFETCH)
OPCODE(1, 2, COMP, DIV)
OPCODE(1, 2, REPL, DP2)
OPCODE(1, 2, OTHR, TXL, .is_tex = 1)
OPCODE(0, 0, NONE, BRK)
OPCODE(0, 1, NONE, IF, .is_branch = 1, .post_indent = 1)
OPCODE(0, 1, NONE, UIF, .is_branch = 1, .post_indent = 1)
OPCODE(1, 2, COMP, READ_INVOC)
OPCODE(0, 0, NONE, ELSE, .is_branch = 1, .pre_dedent = 1, .post_indent = 1)
OPCODE(0, 0, NONE, ENDIF, .pre_dedent = 1)
OPCODE(1, 1, COMP, DDX_FINE)
OPCODE(1, 1, COMP, DDY_FINE)
OPCODE_GAP(81) /* removed */
OPCODE_GAP(82) /* removed */
OPCODE(1, 1, COMP, CEIL)
OPCODE(1, 1, COMP, I2F)
OPCODE(1, 1, COMP, NOT)
OPCODE(1, 1, COMP, TRUNC)
OPCODE(1, 2, COMP, SHL)
OPCODE(1, 1, OTHR, BALLOT)
OPCODE(1, 2, COMP, AND)
OPCODE(1, 2, COMP, OR)
OPCODE(1, 2, COMP, MOD)
OPCODE(1, 2, COMP, XOR)
OPCODE_GAP(93) /* removed */
OPCODE(1, 2, OTHR, TXF, .is_tex = 1)
OPCODE(1, 2, OTHR, TXQ, .is_tex = 1)
OPCODE(0, 0, NONE, CONT)
OPCODE(0, 1, NONE, EMIT)
OPCODE(0, 1, NONE, ENDPRIM)
OPCODE(0, 0, NONE, BGNLOOP, .is_branch = 1, .post_indent = 1)
OPCODE(0, 0, NONE, BGNSUB, .post_indent = 1)
OPCODE(0, 0, NONE, ENDLOOP, .is_branch = 1, .pre_dedent = 1)
OPCODE(0, 0, NONE, ENDSUB, .pre_dedent = 1)
OPCODE(1, 3, OTHR, ATOMFADD, .is_store = 1)
OPCODE(1, 1, OTHR, TXQS, .is_tex = 1)
OPCODE(1, 1, OTHR, RESQ)
OPCODE(1, 1, COMP, READ_FIRST)
OPCODE(0, 0, NONE, NOP)
OPCODE(1, 2, COMP, FSEQ)
OPCODE(1, 2, COMP, FSGE)
OPCODE(1, 2, COMP, FSLT)
OPCODE(1, 2, COMP, FSNE)
OPCODE(0, 1, OTHR, MEMBAR)
OPCODE_GAP(113) /* removed */
OPCODE_GAP(114) /* removed */
OPCODE_GAP(115) /* removed */
OPCODE(0, 1, NONE, KILL_IF)
OPCODE(0, 0, NONE, END)
OPCODE(1, 3, COMP, DFMA)
OPCODE(1, 1, COMP, F2I)
OPCODE(1, 2, COMP, IDIV)
OPCODE(1, 2, COMP, IMAX)
OPCODE(1, 2, COMP, IMIN)
OPCODE(1, 1, COMP, INEG)
OPCODE(1, 2, COMP, ISGE)
OPCODE(1, 2, COMP, ISHR)
OPCODE(1, 2, COMP, ISLT)
OPCODE(1, 1, COMP, F2U)
OPCODE(1, 1, COMP, U2F)
OPCODE(1, 2, COMP, UADD)
OPCODE(1, 2, COMP, UDIV)
OPCODE(1, 3, COMP, UMAD)
OPCODE(1, 2, COMP, UMAX)
OPCODE(1, 2, COMP, UMIN)
OPCODE(1, 2, COMP, UMOD)
OPCODE(1, 2, COMP, UMUL)
OPCODE(1, 2, COMP, USEQ)
OPCODE(1, 2, COMP, USGE)
OPCODE(1, 2, COMP, USHR)
OPCODE(1, 2, COMP, USLT)
OPCODE(1, 2, COMP, USNE)
OPCODE(0, 1, NONE, SWITCH)
OPCODE(0, 1, NONE, CASE)
OPCODE(0, 0, NONE, DEFAULT)
OPCODE(0, 0, NONE, ENDSWITCH)

OPCODE(1, 3, OTHR, SAMPLE)
OPCODE(1, 2, OTHR, SAMPLE_I)
OPCODE(1, 3, OTHR, SAMPLE_I_MS)
OPCODE(1, 4, OTHR, SAMPLE_B)
OPCODE(1, 4, OTHR, SAMPLE_C)
OPCODE(1, 4, OTHR, SAMPLE_C_LZ)
OPCODE(1, 5, OTHR, SAMPLE_D)
OPCODE(1, 4, OTHR, SAMPLE_L)
OPCODE(1, 3, OTHR, GATHER4)
OPCODE(1, 2, OTHR, SVIEWINFO)
OPCODE(1, 2, OTHR, SAMPLE_POS)
OPCODE(1, 2, OTHR, SAMPLE_INFO)
OPCODE(1, 1, COMP, UARL)
OPCODE(1, 3, COMP, UCMP)
OPCODE(1, 1, COMP, IABS)
OPCODE(1, 1, COMP, ISSG)
OPCODE(1, 2, OTHR, LOAD)
OPCODE(1, 2, OTHR, STORE, .is_store = 1)
OPCODE(1, 1, OTHR, IMG2HND)
OPCODE(1, 1, OTHR, SAMP2HND, .is_tex = 1)
OPCODE_GAP(165) /* removed */
OPCODE(0, 0, OTHR, BARRIER)

OPCODE(1, 3, OTHR, ATOMUADD, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMXCHG, .is_store = 1)
OPCODE(1, 4, OTHR, ATOMCAS, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMAND, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMOR, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMXOR, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMUMIN, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMUMAX, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMIMIN, .is_store = 1)
OPCODE(1, 3, OTHR, ATOMIMAX, .is_store = 1)
OPCODE(1, 3, OTHR, TEX2, .is_tex = 1)
OPCODE(1, 3, OTHR, TXB2, .is_tex = 1)
OPCODE(1, 3, OTHR, TXL2, .is_tex = 1)
OPCODE(1, 2, COMP, IMUL_HI)
OPCODE(1, 2, COMP, UMUL_HI)
OPCODE(1, 3, OTHR, TG4, .is_tex = 1)
OPCODE(1, 2, OTHR, LODQ, .is_tex = 1)
OPCODE(1, 3, COMP, IBFE)
OPCODE(1, 3, COMP, UBFE)
OPCODE(1, 4, COMP, BFI)
OPCODE(1, 1, COMP, BREV)
OPCODE(1, 1, COMP, POPC)
OPCODE(1, 1, COMP, LSB)
OPCODE(1, 1, COMP, IMSB)
OPCODE(1, 1, COMP, UMSB)
OPCODE(1, 1, OTHR, INTERP_CENTROID)
OPCODE(1, 2, OTHR, INTERP_SAMPLE)
OPCODE(1, 2, OTHR, INTERP_OFFSET)
OPCODE(1, 1, COMP, F2D)
OPCODE(1, 1, COMP, D2F)
OPCODE(1, 1, COMP, DABS)
OPCODE(1, 1, COMP, DNEG)
OPCODE(1, 2, COMP, DADD)
OPCODE(1, 2, COMP, DMUL)
OPCODE(1, 2, COMP, DMAX)
OPCODE(1, 2, COMP, DMIN)
OPCODE(1, 2, COMP, DSLT)
OPCODE(1, 2, COMP, DSGE)
OPCODE(1, 2, COMP, DSEQ)
OPCODE(1, 2, COMP, DSNE)
OPCODE(1, 1, COMP, DRCP)
OPCODE(1, 1, COMP, DSQRT)
OPCODE(1, 3, COMP, DMAD)
OPCODE(1, 1, COMP, DFRAC)
OPCODE(1, 2, COMP, DLDEXP)
OPCODE(2, 1, REPL, DFRACEXP)
OPCODE(1, 1, COMP, D2I)
OPCODE(1, 1, COMP, I2D)
OPCODE(1, 1, COMP, D2U)
OPCODE(1, 1, COMP, U2D)
OPCODE(1, 1, COMP, DRSQ)
OPCODE(1, 1, COMP, DTRUNC)
OPCODE(1, 1, COMP, DCEIL)
OPCODE(1, 1, COMP, DFLR)
OPCODE(1, 1, COMP, DROUND)
OPCODE(1, 1, COMP, DSSG)
OPCODE(1, 1, COMP, VOTE_ANY)
OPCODE(1, 1, COMP, VOTE_ALL)
OPCODE(1, 1, COMP, VOTE_EQ)
OPCODE(1, 2, COMP, U64SEQ)
OPCODE(1, 2, COMP, U64SNE)
OPCODE(1, 2, COMP, I64SLT)
OPCODE(1, 2, COMP, U64SLT)
OPCODE(1, 2, COMP, I64SGE)
OPCODE(1, 2, COMP, U64SGE)
OPCODE(1, 2, COMP, I64MIN)
OPCODE(1, 2, COMP, U64MIN)
OPCODE(1, 2, COMP, I64MAX)
OPCODE(1, 2, COMP, U64MAX)
OPCODE(1, 1, COMP, I64ABS)
OPCODE(1, 1, COMP, I64SSG)
OPCODE(1, 1, COMP, I64NEG)
OPCODE(1, 2, COMP, U64ADD)
OPCODE(1, 2, COMP, U64MUL)
OPCODE(1, 2, COMP, U64SHL)
OPCODE(1, 2, COMP, I64SHR)
OPCODE(1, 2, COMP, U64SHR)
OPCODE(1, 2, COMP, I64DIV)
OPCODE(1, 2, COMP, U64DIV)
OPCODE(1, 2, COMP, I64MOD)
OPCODE(1, 2, COMP, U64MOD)
OPCODE(1, 2, COMP, DDIV)
OPCODE(1, 3, OTHR, LOD)
