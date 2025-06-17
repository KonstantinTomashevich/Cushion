# Cushion

Cushion is a **partial** C preprocessor for softening the code before passing it to custom code generation software.

> **Warning:**
> Cushion is still in early development and not test on a big enough project.
> We plan to test it on [Kan project](https://github.com/KonstantinTomashevich/Kan) first and it might result in lots 
> of changes if something is wrong in current implementation.

> **Warning:**
> Make sure that you've read Limitations section near the end of readme before considering to use this tool and/or 
> creating issues.

## License

![LGPL3 license logo](./.readme/license.png)

## Motivation

C is a very easy to parse language comparing to most modern languages, therefore it is possible to extend C language
on per-project basis using custom code generation tools: for example, custom reflection generation or boilerplate
code generation software. 

However, almost always there is one huge obstacle to doing so: preprocessing. As C code is designed to be preprocessed
before compiling, it is impossible to write such parser in code generator that will be able to properly understand
everything that is going on without doing actual preprocessing (macro could be anywhere and you don't know what it is 
until preprocessing). Of course, it is possible to use preprocessor-only mode from the compiler, but it has its own 
issues discussed below.

Cushion is a preprocessor that is aimed to be used to prepare code for code generation tools pass and therefore is much
more convenient for this purpose than inbuilt preprocessors.

### Inbuilt preprocessors: issues

By using inbuilt preprocessors we mean running `gcc -E`, `cl /P /Fi` and so on. However, usage of standalone executables
like `cpp` and `clang-cpp` has the same issues as it is essentially the same preprocessors, but in different 
executable.

- Postprocessor output is not standardized, therefore it is slightly different in different compilers. As long as you
  target only one compiler, it is okay, but when you target several ones, for example GCC and MSVC, issues start to
  appear. For example, different line markup, different pragma markup, compiler-specific things like attributes.
  Properly parsing it in your code generation software might become painful over time.

- Postprocessor output does not usually contain all the context needed to properly compile code. In other words,
  compilers usually do not expect to be given fully preprocessed code and properly operate on it without some metadata
  from previous stages. In practice, it may result in errors like: clang will spam warnings about GNU like markup 
  despite the fact that its preprocessor produced that markup, a bunch of warnings from system or std headers may appear
  as compiler no longer knows that it is part of system or std headers (always happens if you force clang preprocessor 
  to use standard line markup instead of GNU). It is not a tragedy, but existence if this issues makes me worry that 
  some more serious issues due to this logic might appear down the road.

- Integrating inbuilt preprocessor execution into your build system might be a complex task. With Ninja generator in
  CMake it is possible to just create separate target and add necessary compile options to it, however it somehow
  makes it impossible for CLion to load project on Windows -- highlight is gone everywhere, but on Linux everything
  is fine. It also breaks dependency management for compilation on Windows: preprocessor adds line markup with 
  things like `"<built-in>"` and `"<command line>"` and Ninja somehow treats them as dependencies, however they do not
  really exist and Ninja just compiles the file from scratch every build due to missing dependencies. And Visual Studio 
  generator situation is worse as just adding flags somehow does not work and everything requires whole bucket of custom
  handling [like this](https://github.com/KonstantinTomashevich/CMakeUnitFramework/blob/ee96341/Unit.cmake#L484). 
  I haven't tried it on other generators and IDEs, however I'm almost certain they will have their own quirks.

- Resolving all includes, especially system ones, is generally inefficient when you're running code generation for
  your own project: your code generator ends up diving into loads of code where significant portion of the code
  is just not relevant for the code generation. So, if your project is not that big, but includes system headers,
  your code generator will end up searching for actual code like these potatoes are searching for soil:

<img src="./.readme/where_is_the_fucking_soil.jpg" width="300"/>

It is possible to deal with these issues through different custom fixes, but all these issues raise one concern:
shouldn't it be better to just use custom preprocessor tailored for the job?

### How Cushion deals with it

Now lets talk about how Cushion approaches the job and how it solves most of the issues.

Cushion **does not do full preprocessing**: its goal is to only make code properly readable for code generators and then
this code would pass through actual preprocessing inside your compiler once again. In details:

- It does not perform full include, instead tier-based include handling is used: using command line,
  full-include (do proper include) and scan-only-include (only scan for macros, do not do anything else) paths are 
  provided. Other includes are just left as it is. That means that there would not be any unexpected code in the 
  output, which makes life for code generators easier and fixes system-header-bleeding issue described above.

- It only cares about conditional compilation, macro definitions and macro replacement, it does not touch pragmas,
  errors, warnings, embeds from new standard and so on.

- It can be disabled on code region level using special defines. It can also be forbidden from unwrapping some macros.

Therefore:

- Cushion output is standardized and predictable on any platform, no more compiler-specific handling in code generators.

- It does not capture excessive context unless told to by not resolving includes unless explicitly told to do it,
  so no more unexpected warnings from compiler in headers that are not yours. It also makes sure that code generator
  context is not bloated.

- It is simple to integrate it into build system as a usual custom command with custom cmake depfile, which is produced 
  by Cushion too. Therefore, it requires no compiler-specific handling.

## Non-standard preprocessing extensions

I've been using custom code generation on 
[my game engine project (work in progress)](https://github.com/KonstantinTomashevich/Kan) and I've gathered some
patterns that might be useful on preprocessor level for code generation. Therefore, I've decided to extend preprocessing
with some features. They're completely optional and their usage without passing enabling command line options to Cushion 
command would result in errors.

### Defer blocks

`CUSHION_DEFER { ... your code ... }` makes it possible to write blocks of code that are executed on scope exit through
natural means: `return`, `break`, `continue`, `goto` or just scope end. It also makes sure that `return` value is 
calculated before executing defers and all defers are executed in reverse order. That makes RAII possible (which is a 
very cool feature of C++) and greatly simplifies handling of locks and internal data with lifecycle inside function.
Also, this feature can be used inside regular macros, making something like 
`#define LOCK_GUARD(LOCK) do_lock (LOCK); CUSHION_DEFER { do_unlock (LOCK); }` possible.

### Wrapper macros

Cushion introduces `__CUSHION_WRAPPED__` identifier for usage inside macros. If macro uses `__CUSHION_WRAPPED__`, then
it becomes wrapper macro: wrapper macro must always be followed by block of code in curly braces, that is then
inserted in place of `__CUSHION_WRAPPED__` identifiers (enclosing curly braces are not inserted). This is very useful 
when we need to embed some logic inside complex construction like some complex iteration. For example, if you're 
iterating on something with database-like API, you need to properly manage cursor, access handle, perhaps something 
else too. Which is usually the same almost everywhere in your code, so it will be a good place for macro like that:

```c
#define DATABASE_QUERY(VALUE_TYPE, QUERY, PARAMETER)                                                                   \
    {                                                                                                                  \
        database_read_cursor_t cursor = database_query_execute_read (QUERY, PARAMETER);                                \
        CUSHION_DEFER { database_read_cursor_close (cursor); }                                                         \
                                                                                                                       \
        while (1)                                                                                                      \
        {                                                                                                              \
            database_read_access_t access = database_read_cursor_resolve (cursor);                                     \
            database_read_cursor_advance (cursor);                                                                     \
            CUSHION_DEFER { database_read_access_close (access); }                                                     \
            const struct VALUE_TYPE *value = database_read_access_resolve (access);                                    \
                                                                                                                       \
            if (value)                                                                                                 \
            {                                                                                                          \
                __CUSHION_WRAPPED__                                                                                    \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                /* No more values in query cursor. */                                                                  \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
    }
```

As you can see, we can combine power of defer and wrapped macros here in order to produce convenient reusable syntax 
sugar for executing arbitrary code inside queries. Moreover, because of defer blocks, it is safe to return whatever we
need from wrapped code if we need to or to just break out of query: cursor and access will be properly closed. Just 
don't return value pointer like that, databases APIs do not expect such behavior for sure.

There is an argument that similar result could be achieved through macro with variadic arguments. It is true and it is
totally possible, however passing everything to usual macro as variadic arguments results in loss of line information, 
making it very difficult to fix compilation errors and making it impossible to debug properly. 
`__CUSHION_WRAPPED__` is guaranteed to save lines inside wrapped block, making fixing compilation and debugging as easy 
as with usual code block.

Also, it is allowed to use directives like `#define`, `#line` and `#undef` inside `__CUSHION_WRAPPED__` blocks: they
will be correctly unwrapped there. But beware that `__CUSHION_WRAPPED__` might be pasted several times, therefore if
you use `#define`, you also need to use `#undef` for the same macro in order to avoid duplication.

### Statement accumulators

Statement accumulators are a framework for conditionally generating code in one place from another place. This feature
consists of several commands that tell Cushion what to do:

- `CUSHION_STATEMENT_ACCUMULATOR (statement_accumulator_identifier)` defines a named place in the code where
  generated statements will be written after full preprocessing pass.

- `CUSHION_STATEMENT_ACCUMULATOR_PUSH (statement_accumulator_identifier, options...) { ... your code ... }`
  pushes code into accumulator with given identifier, essentially generating new code in that place. Comma separated 
  options provide more control on how statement is pushed. Supported options are:

    - `unique` -- given code is compared to the statements that were already pushed. If other exactly equal code was
      already pushed, then this push is skipped.

    - `optional` -- does not generate error when accumulator does not exist. In combination with `unordered`, does
      not generate error when processing is finished, but there is no suitable accumulator.

    - `unordered` -- if there is no accumulator, push is saved for later and applied when accumulator with required
      name is found. Signals error when processing is finished and accumulator is not found unless `optional` option
      is specified.

- `CUSHION_STATEMENT_ACCUMULATOR_REF (reference_identifier, source_identifier)` defines a reference to
  statement accumulator, which can be used instead of an actual accumulator in push command. It can also receive
  `unordered` pushes if any. Reference cannot point to each other, only to real accumulators, for the sake of 
  simplicity. The goal of references is to provide dynamic abstracted context for pushing that can be changed from 
  outside when needed without being explicitly specified in push command.

- `CUSHION_STATEMENT_ACCUMULATOR_UNREF (reference_identifier)` destroys a reference with given name.

To understand how it can be used, lets return to the database example from the above. Let's say that we want queries
for different value types to be automatically generated in some translation-unit-local context structure. We can
achieve it by expanding our macro to use pushes:

```c
#define DATABASE_QUERY(VALUE_TYPE, PARAMETER)                                                                          \
    {                                                                                                                  \
        CUSHION_STATEMENT_ACCUMULATOR_PUSH (database_context_accumulator, unique)                                      \
        { database_query_t query_##VALUE_TYPE; }                                                                       \
                                                                                                                       \
        database_read_cursor_t cursor = database_query_execute_read (database_context->query_##VALUE_TYPE, PARAMETER); \
        CUSHION_DEFER { database_read_cursor_close (cursor); }                                                         \
                                                                                                                       \
        while (1)                                                                                                      \
        {                                                                                                              \
            database_read_access_t access = database_read_cursor_resolve (cursor);                                     \
            database_read_cursor_advance (cursor);                                                                     \
            CUSHION_DEFER { database_read_access_close (access); }                                                     \
            const struct VALUE_TYPE *value = database_read_access_resolve (access);                                    \
                                                                                                                       \
            if (value)                                                                                                 \
            {                                                                                                          \
                __CUSHION_WRAPPED__                                                                                    \
            }                                                                                                          \
            else                                                                                                       \
            {                                                                                                          \
                /* No more values in query cursor. */                                                                  \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
    }
```

In the macro above, we expect that context structure is always accessible under `database_context` variable, which is
usually the case for the most functions (at least in my experience). Then we can use it like that:

```c
struct database_context_1_t
{
    CUSHION_STATEMENT_ACCUMULATOR (database_context_1)
};

struct database_context_2_t
{
    CUSHION_STATEMENT_ACCUMULATOR (database_context_2)
};

CUSHION_STATEMENT_ACCUMULATOR_REF (database_context_accumulator, database_context_1)

void database_function_1_a (struct database_context_1_t *database_context)
{
    DATABASE_QUERY (value_type_a, 1)
    {
        // Do what you need.
    }

    DATABASE_QUERY (value_type_b, 2)
    {
        // Do what you need.
    }

    DATABASE_QUERY (value_type_a, 3)
    {
        // Do what you need.
    }
}

void database_function_1_b (struct database_context_1_t *database_context)
{
    DATABASE_QUERY (value_type_b, 1)
    {
        // Do what you need.
    }

    DATABASE_QUERY (value_type_a, 4)
    {
        // Do what you need.
    }
}

CUSHION_STATEMENT_ACCUMULATOR_REF (database_context_accumulator, database_context_2)

void database_function_2 (struct database_context_2_t *database_context)
{
    DATABASE_QUERY (value_type_x, 10)
    {
        // Do what you need.
    }

    DATABASE_QUERY (value_type_y, 20)
    {
        // Do what you need.
    }
}
```

In practice, this pattern is very useful for data-driven game programming patterns like ECS if access to world data from
systems is implemented in database-like fashion to ensure safe and correct multithreading. Also, this example omits the
need to properly initialize and shutdown queries, but it can be easily achieved by adding accumulators and pushes
for init and shutdown code.

Main limitation of this feature is that you cannot use `CUSHION_STATEMENT_ACCUMULATOR` in headers as you're most likely
not going to include preprocessed header anywhere. But in practice it is rarely needed as accumulators are mostly used
for private structure generation or for private code.

One important note is that usage of statement accumulators prevents code from being written to output right away as
statement accumulators can only be resolved when preprocessing is finished, therefore it increases memory footprint.
But it shouldn't be an issue in most cases as it is very unlikely that you'll need to preprocess gigabytes of code and
even if you do, you'll have lots of other issues to solve already.

## Limitations

Right now Cushion is more of a hobby project and is not a heavy-production-ready thing. Therefore, current 
implementation has several limitations:

- Unicode characters are not fully supported: they are only supported in strings and comments.
  The reason is that re2c drowns in big character classes and becomes very slow.
- Universal character names are not supported.
- Only simple escape sequences (that is the term from specification) are supported.
- Nothing related to ISO/IEC 646 is supported.
- `typeof` from C23 is used to properly generate returns for `CUSHION_DEFER`. It is possible to guess type for temporary
  storing return expression result before executing defers without `typeof`, but it is difficult and error-prone, 
  therefore `typeof` is used for now as it is supported by most popular compilers.
- There might be some bugs and some non-standard-complying behaviours, unfortunately. Not intentionally, but due to lack
  of time and lack of skills (author is not a profession compiler/parser developer and only did these things as hobby 
  projects). Also, this tool is only properly used right now on 
  [Kan project](https://github.com/KonstantinTomashevich/Kan), therefore different projects might encounter different
  errors that weren't found on Kan.

These limitations will be addressed on per-request basis when it is really needed and when author has enough time to 
do it properly.

## Acknowledgement

This project would be quite difficult to build without [re2c](https://re2c.org/), which greatly simplifies building
anything that needs tokenization and parsing.
