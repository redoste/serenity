## Name

The Shell Command Language

## Introduction

The shell operates according to the following general steps:

* Some string is read from a source, be it a file, the standard input, or a command string (see [`Shell`(1)](../man1/Shell.md))
* The shell parses the input to an abstract syntax tree
* The shell performs various expansions and/or resolutions on the nodes
* The shell performs various type checks and syntactic checks
* The shell interprets the AST, evaluating commands as needed
* For each given command, the shell flattens all the string/list arguments
* For each given command, the shell records the applicable redirections
* Should a command be executed, the shell applies the redirections, and executes the command with the flattened argument list
* Should a command need waiting, the shell shall wait for the command to finish, and continue execution

Any text below is superceded by the formal grammar defined in the _formal grammar_ section.

## General Token Recognition

This section describes the general tokens the language accepts, it should be noted that due to nature of the language, some tokens are valid only in a specific context.

##### Bareword
String of characters that are not _Special_ or _Syntactic Elements_

##### Glob
String of characters containing at least one of `*?` in _bareword_ position

##### Single Quoted String
Any sequence of characters between two single quotes (`'`)

##### Double Quoted String
Any sequence of _Double Quoted String Part_ tokens:
* Barewords
* Single Quotes
* Variable References
* Evaluate expressions
* Escaped sequences

##### Variable Reference
Any sequence of _Identifier_ characters, or a _Special Variable_ follwing a `$`

##### Evaluate expression
Any expression following a `$` that is not a variable reference:
* Inline execution: A _syntactic list_ following a `$`:
* Dynamic evaluation: Any other expression following a `$`

##### Lists
Any two expressions joined by the Join operator (` ` [whitespace]), or a _variable reference_ referring to a list value
* Syntactic Lists: Any _list_ enclosed in parentheses (`(` and `)`)

##### Comments
Any text following a `#` in _bareword_ position, up to but not including a newline

##### Keywords
The following tokens:
* `for` in command name position
* `in` as a syntactic element of a `for` expression
* `if` in command name position, or after the `else` keyword
* `else` after a partial `if` expression

##### Special characters
Any of the following:
* `;` in bareword position
* `\\n` (a newline) in _bareword_ position
* Any of `(){}`
* Any of `*?` not in _glob_ position

##### Tilde
Any initial path segment starting with the character `~` in _bareword_ position, Optionally followed by a _bareword_ for the username

## Redirections
The shell can create various redirections to file descriptors of a command before executing it, the general syntax for redirections is an optional file descriptor, followed by a redirection operator, followed by a destination.

There are four redirection operators corresponding to various file descriptor open modes: `Read`, `Write`, `WriteAppend` and `ReadWrite`, respectively `<`, `>`, `>>` and `<>`.

A special syntactic element `&fd` can reference a file descriptor as a destination.

Redirections take two main forms, Read/Write redirections, and fd closure redirections.
##### Read/Write
* Allowed operators: all
* Allowed destinations: file paths (any shell _expression_) and _file descriptor references_

##### Close
* Allowed operators: `Write` (`>`)
* Allowed destinations: the special "close" reference `&-`

#### Examples
```sh
# Redirect the standard error to a file, and close the standard input
$ 2> foo 1>&-

# Redirect a file as read-write into the standard input
$ 1<>foo

# Redirect the standard output to /dev/null
$ >/dev/null
```

## Expansions
The shell performs various expansions, in different stages.

* Glob Expansion: Globs shall be expanded to a list.

* Variable Expansion: Variables shall be expanded preserving their types.

* Juxtaposition Expansion: Juxtapositions shall be expanded as list products.

* Other expansions: Tildes, Evaluate expressions, etc. shall be expanded as needed.

### Juxtapositions
Any two expressions joined without any operator are considered to be in a Juxtaposition, with the resulting value being the list product of two expressions.
For instance, `(1 2)(3 4)` shall be evaluated to `(13 14 23 24)` by calculating the list product of the two expressions `(1 2)` and `(3 4)`.

### Tildes
Any bareword starting with a tilde (`~`) and spanning up to the first path separator (`/`) - or EOL - is considered to be a tilde expansion with the text between the tilde and the separator being the _username_, which shall be expanded to a single string containing the home directory of the given _username_ (or the current user if no username is provided).

### Evaluate
Evaluate expressions take the general form of a dollar sign (`$`) followed by some _expression_, which is evaluated by the rules below.
- Should the _expression_ be a string, it shall be evaluated as a dynamic variable lookup by first evaluating the string, and then looking up the given variable.
- Should the _expression_ be a list or a command, it shall be converted to a command, whose output (from the standard output) shall be captured, and split to a list with the shell local variable `IFS` (or the default splitter `\n` (newline, 0x0a)). It should be noted that the shell option `inline_exec_keep_empty_segments` will determine whether empty segments in the split list shall be preserved when this expression is evaluated, this behaviour is disabled by default.

## Commands

A `Command` is a single simple command, containing arguments and redirections for a single program, or a compound command containing a shell control structure. The shell can evaluate a sequence of commands, a conditional relation between commands, or various semantic elements composed of commands and intrinsics.

Commands can be either calls to Shell builtins, or external programs.

## Shell Semantic Elements
The commands can be composed into semantic elements, producing composite commands:

### Sequences
A sequence of commands, executed serially independent of each other: `Commanad ; Command ; Command ...` 

It should be noted that a newline (`\\n`) can be substituted for the semicolon (`;`).

#### Example
```sh
# Do one thing, then do another
echo foo; echo bar
```

### Logical Relations
A sequence of commands whose execution depends somehow on the result of another

#### `Command && Command && Command ...` (AND)
Short-circuiting command evaluations, will cancel the entire chain should any command fails (have a non-zero exit code)

#### `Command || Command || Command ...` (OR)
Short-circuiting command evaluation, will continue down the chain if any command fails.

It should be noted that `And` chains bind more tightly than `Or` chains, so an expression of the form `C1 && C2 || C3` is understood as "evaluate `C1`, if successful, evaluate `C2`, if not successful, evaluate `C3`".

##### Examples
```sh
# Create file if not found
test -f foo.txt || touch foo.txt

# Announce execution status of a command
rm test && echo "deleted!" || echo "failed with $?"
```

#### Control Structures

##### Conditionals
Conditionals can either be expressed with the _Logical Relations_, or via explicit `if` expressions.
An `if` expression contains at least a _condition_ and a _then clause_, and optionally the `else` keyword followed by an _else clause_.
An _else clause_ may contain another `if` expression instead of a normal block.

The _then clause_ **must** be surrounded by braces, but the _else clause_ may also be another `if` expression.

An `if` expression evaluates either the _then clause_ or (if available) the _else clause_, based on the exit code of the _condition_; should the exit code be zero, the _then clause_ will be executed, and if not, the _else clause_ will.

###### Examples
```sh
# Remove a file if it exists, create it otherwise
if test -e the_file {
    rm the_file
} else {
    touch the_file
}

# Cond chain (if-elseif-else)
if A {
    echo A
} else if B {
    echo B
} else {
    echo C
}
```

##### For Loops
For Loops evaluate a sequence of commands once per element in a given list.
The shell has two forms of _for loops_, one with an explicitly named iteration variable, and one with an implicitly named one.
The general syntax follows the form `for name in expr { sequence }`, and allows omitting the `name in` part to implicitly name the variable `it`.

A for-loop evaluates the _sequence_ once per every element in the _expr_, seetting the local variable _name_ to the element being processed.

The Shell shall cancel the for loop if two consecutive commands are interrupted via SIGINT (\^C), and any other terminating signal aborts the loop entirely.

###### Examples
```sh
# Iterate over every non-hidden file in the current directory, and prepend '1-' to its name.
$ for * { mv $it 1-$it }

# Iterate over a sequence and write each element to a file
$ for i in $(seq 1 100) { echo $i >> foo }
```

##### Subshells
Subshells evaluate a given block in a new instance (fork) of the current shell process. to create a subshell, any valid shell code can be enclosed in braces.

###### Examples
```sh
# Run a block of code in the background, in a subshell, then detach it from the current shell
$ { for * { te $it } }&
$ disown
```

## Formal Grammar

### Shell Grammar
```
toplevel :: sequence?

sequence :: variable_decls? or_logical_sequence terminator sequence
          | variable_decls? or_logical_sequence '&' sequence
          | variable_decls? or_logical_sequence
          | variable_decls? terminator sequence

or_logical_sequence :: and_logical_sequence '|' '|' and_logical_sequence
                     | and_logical_sequence

and_logical_sequence :: pipe_sequence '&' '&' and_logical_sequence
                      | pipe_sequence

terminator :: ';'
            | '\n'

variable_decls :: identifier '=' expression (' '+ variable_decls)? ' '*
                | identifier '=' '(' pipe_sequence ')' (' '+ variable_decls)? ' '*

pipe_sequence :: command '|' pipe_sequence
               | command
               | control_structure '|' pipe_sequence
               | control_structure

control_structure :: for_expr
                   | if_expr
                   | subshell

for_expr :: 'for' ws+ (identifier ' '+ 'in' ws*)? expression ws+ '{' toplevel '}'

if_expr :: 'if' ws+ or_logical_sequence ws+ '{' toplevel '}' else_clause?

else_clause :: else '{' toplevel '}'
             | else if_expr

subshell :: '{' toplevel '}'

command :: redirection command
         | list_expression command?

redirection :: number? '>'{1,2} ' '* string_composite
             | number? '<' ' '* string_composite
             | number? '>' '&' number
             | number? '>' '&' '-'

list_expression :: ' '* expression (' '+ list_expression)?

expression :: evaluate expression?
            | string_composite expression?
            | comment expession?
            | '(' list_expression ')' expression?

evaluate :: '$' '(' pipe_sequence ')'
          | '$' expression          {eval / dynamic resolve}

string_composite :: string string_composite?
                  | variable string_composite?
                  | bareword string_composite?
                  | glob string_composite?

string :: '"' dquoted_string_inner '"'
        | "'" [^']* "'"

dquoted_string_inner :: '\' . dquoted_string_inner?       {concat}
                      | variable dquoted_string_inner?    {compose}
                      | . dquoted_string_inner?
                      | '\' 'x' digit digit dquoted_string_inner?
                      | '\' [abefrn] dquoted_string_inner?

variable :: '$' identifier
          | '$' '$'
          | '$' '?'
          | '$' '*'
          | '$' '#'
          | ...

comment :: '#' [^\n]*

bareword :: [^"'*$&#|()[\]{} ?;<>] bareword?
          | '\' [^"'*$&#|()[\]{} ?;<>] bareword?

bareword_with_tilde_expansion :: '~' bareword?

glob :: [*?] bareword?
      | bareword [*?]

digit :: <native hex digit>
number :: <number in base 10>
identifier :: <string of word characters>
```
