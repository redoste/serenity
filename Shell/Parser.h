/*
 * Copyright (c) 2020, the SerenityOS developers.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "AST.h"
#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>

class Parser {
public:
    Parser(StringView input)
        : m_input(move(input))
    {
    }

    RefPtr<AST::Node> parse();

private:
    RefPtr<AST::Node> parse_toplevel();
    RefPtr<AST::Node> parse_sequence();
    RefPtr<AST::Node> parse_and_logical_sequence();
    RefPtr<AST::Node> parse_or_logical_sequence();
    RefPtr<AST::Node> parse_variable_decls();
    RefPtr<AST::Node> parse_pipe_sequence();
    RefPtr<AST::Node> parse_command();
    RefPtr<AST::Node> parse_control_structure();
    RefPtr<AST::Node> parse_for_loop();
    RefPtr<AST::Node> parse_if_expr();
    RefPtr<AST::Node> parse_subshell();
    RefPtr<AST::Node> parse_redirection();
    RefPtr<AST::Node> parse_list_expression();
    RefPtr<AST::Node> parse_expression();
    RefPtr<AST::Node> parse_string_composite();
    RefPtr<AST::Node> parse_string();
    RefPtr<AST::Node> parse_doublequoted_string_inner();
    RefPtr<AST::Node> parse_variable();
    RefPtr<AST::Node> parse_evaluate();
    RefPtr<AST::Node> parse_comment();
    RefPtr<AST::Node> parse_bareword();
    RefPtr<AST::Node> parse_glob();

    template<typename A, typename... Args>
    NonnullRefPtr<A> create(Args... args);

    bool at_end() const { return m_input.length() <= m_offset; }
    char peek();
    char consume();
    void putback();
    bool expect(char);
    bool expect(const StringView&);

    StringView consume_while(Function<bool(char)>);

    struct ScopedOffset {
        ScopedOffset(Vector<size_t>& offsets, size_t offset)
            : offsets(offsets)
            , offset(offset)
        {
            offsets.append(offset);
        }
        ~ScopedOffset()
        {
            auto last = offsets.take_last();
            ASSERT(last == offset);
        }

        Vector<size_t>& offsets;
        size_t offset;
    };

    OwnPtr<ScopedOffset> push_start();

    StringView m_input;
    size_t m_offset { 0 };
    Vector<size_t> m_rule_start_offsets;
};

#if 0
constexpr auto the_grammar = R"(
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
)";
#endif
