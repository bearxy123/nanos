union tuple;
typedef union tuple *tuple;

typedef closure_type(tuple_generator, tuple);
typedef closure_type(tuple_get, value, symbol);
typedef closure_type(tuple_set, void, symbol, value);
typedef closure_type(binding_handler, boolean, value, value);
typedef closure_type(tuple_iterate, boolean, binding_handler);

typedef struct function_tuple {
    tuple_get g;
    tuple_set s;
    tuple_iterate i;
} *function_tuple;

union tuple {
    struct table t;
    struct function_tuple f;
};

value get(value e, symbol a);
void set(value e, symbol a, value v);
boolean iterate(value e, binding_handler h);

void init_tuples(heap theap);
int tuple_count(tuple t);
symbol tuple_get_symbol(tuple t, value v);
tuple allocate_tuple();
void destruct_tuple(tuple t, boolean recursive);
void deallocate_value(tuple t);

void encode_tuple(buffer dest, table dictionary, tuple t, u64 *total);

// h is for the bodies, the space for symbols and tuples are both implicit
void *decode_value(heap h, table dictionary, buffer source, u64 *total,
                   u64 *obsolete);
void encode_eav(buffer dest, table dictionary, tuple e, symbol a, value v,
                u64 *obsolete);

static inline boolean is_tuple(value v)
{
    value_tag tag = tagof(v);
    return tag == tag_table_tuple || tag == tag_function_tuple;
}

static inline boolean is_symbol(value v)
{
    return tagof(v) == tag_symbol;
}

static inline boolean is_string(value v)
{
    return tagof(v) == tag_unknown; // XXX tag_string
}

// seriously reconsider types allowed in tuples.. in particular simple
// ints have an anambiguous translation back and forth to strings (?)
static inline boolean u64_from_value(value v, u64 *result)
{
    return parse_int(alloca_wrap((buffer)v), 10, result);
}

static inline value value_from_u64(heap h, u64 n)
{
    value result = allocate_buffer(h, 10);
    print_number((buffer)result, n, 10, 0);
    return result;
}

static inline value value_rewrite_u64(value v, u64 n)
{
    assert(!is_tuple(v));
    buffer_clear((buffer)v);
    print_number((buffer)v, n, 10, 0);
    return v;
}

/* XXX questionable part of interface */
static inline tuple find_or_allocate_tuple(tuple t, symbol s)
{
    value v = get(t, s);
    assert(v != INVALID_ADDRESS);
    if (!v)
        return allocate_tuple();
    assert(is_tuple(v));
    return (tuple)v;
}

/* get and validate that result is a tuple type */
static inline tuple get_tuple(value e, symbol a)
{
    value v = get(e, a);
    return (v && is_tuple(v)) ? v : 0;
}

/* TODO - change to validate string tag type */
static inline string get_string(value e, symbol a)
{
    value v = get(e, a);
    return (v && tagof(v) == tag_unknown) ? v : 0;
}

/* TODO - change to validate number type */
static inline string get_number(value e, symbol a)
{
    return get_string(e, a);
}

static inline boolean get_u64(value e, symbol a, u64 *result)
{
    string s = get_number(e, a);
    if (!s)
        return false;
    return u64_from_value(s, result);
}

/* really just for parser output */
static inline boolean is_null_string(value v)
{
    return is_string(v) && buffer_length(v) == 0;
}
