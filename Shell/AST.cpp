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

#include "AST.h"
#include "Shell.h"
#include <AK/MemoryStream.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/URL.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <signal.h>

//#define EXECUTE_DEBUG

namespace AST {

template<typename T, typename... Args>
static inline NonnullRefPtr<T> create(Args... args)
{
    return adopt(*new T(args...));
}

template<typename T>
static inline NonnullRefPtr<T> create(std::initializer_list<NonnullRefPtr<Value>> arg)
{
    return adopt(*new T(arg));
}

static inline void print_indented(const String& str, int indent)
{
    for (auto i = 0; i < indent; ++i)
        dbgprintf("  ");
    dbgprintf("%s\n", str.characters());
}

static inline Vector<Command> join_commands(Vector<Command> left, Vector<Command> right)
{
    Command command;

    auto last_in_left = left.take_last();
    auto first_in_right = right.take_first();

    command.argv.append(last_in_left.argv);
    command.argv.append(first_in_right.argv);

    command.redirections.append(last_in_left.redirections);
    command.redirections.append(first_in_right.redirections);

    command.should_wait = first_in_right.should_wait && last_in_left.should_wait;
    command.is_pipe_source = first_in_right.is_pipe_source;
    command.should_notify_if_in_background = first_in_right.should_notify_if_in_background || last_in_left.should_notify_if_in_background;

    Vector<Command> commands;
    commands.append(left);
    commands.append(command);
    commands.append(right);

    return commands;
}

void Node::for_each_entry(RefPtr<Shell> shell, Function<IterationDecision(RefPtr<Value>)> callback)
{
    auto value = run(shell)->resolve_without_cast(shell);
    if (value->is_job()) {
        callback(value);
        return;
    }

    if (value->is_list_without_resolution()) {
        auto list = value->resolve_without_cast(shell);
        for (auto& element : static_cast<ListValue*>(list.ptr())->values()) {
            if (callback(element) == IterationDecision::Break)
                break;
        }
        return;
    }

    auto list = value->resolve_as_list(shell);
    for (auto& element : list) {
        if (callback(create<StringValue>(move(element))) == IterationDecision::Break)
            break;
    }
}

Vector<Command> Node::to_lazy_evaluated_commands(RefPtr<Shell> shell)
{
    if (would_execute()) {
        // Wrap the node in a "should immediately execute next" command.
        return {
            Command { {}, {}, true, false, true, true, {}, { NodeWithAction(*this, NodeWithAction::Sequence) } }
        };
    }

    return run(shell)->resolve_as_commands(shell);
}

void Node::dump(int level) const
{
    print_indented(String::format("%s at %d:%d", class_name().characters(), m_position.start_offset, m_position.end_offset), level);
}

Node::Node(Position position)
    : m_position(position)
{
}

Vector<Line::CompletionSuggestion> Node::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (matching_node) {
        if (matching_node->is_bareword()) {
            auto corrected_offset = offset - matching_node->position().start_offset;
            auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

            if (corrected_offset > node->text().length())
                return {};
            auto& text = node->text();

            // If the literal isn't an option, treat it as a path.
            if (!(text.starts_with("-") || text == "--" || text == "-"))
                return shell.complete_path("", text, corrected_offset);

            // If the literal is an option, we have to know the program name
            // should we have no way to get that, bail early.

            if (!hit_test_result.closest_command_node)
                return {};

            auto program_name_node = hit_test_result.closest_command_node->leftmost_trivial_literal();
            if (!program_name_node)
                return {};

            String program_name;
            if (program_name_node->is_bareword())
                program_name = static_cast<BarewordLiteral*>(program_name_node.ptr())->text();
            else
                program_name = static_cast<StringLiteral*>(program_name_node.ptr())->text();

            return shell.complete_option(program_name, text, corrected_offset);
        }
        return {};
    }
    auto result = hit_test_position(offset);
    if (!result.matching_node)
        return {};
    auto node = result.matching_node;
    if (node->is_bareword() || node != result.closest_node_with_semantic_meaning)
        node = result.closest_node_with_semantic_meaning;

    if (!node)
        return {};

    return node->complete_for_editor(shell, offset, result);
}

Vector<Line::CompletionSuggestion> Node::complete_for_editor(Shell& shell, size_t offset)
{
    return Node::complete_for_editor(shell, offset, { nullptr, nullptr, nullptr });
}

Node::~Node()
{
}

void And::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> And::run(RefPtr<Shell> shell)
{
    auto commands = m_left->to_lazy_evaluated_commands(shell);
    commands.last().next_chain.append(NodeWithAction { *m_right, NodeWithAction::And });
    return create<CommandSequenceValue>(move(commands));
}

void And::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    metadata.is_first_in_list = true;
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult And::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node) {
        if (!result.closest_command_node)
            result.closest_command_node = m_right;
        return result;
    }
    result = m_right->hit_test_position(offset);
    if (!result.closest_command_node)
        result.closest_command_node = m_right;
    return result;
}

And::And(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

And::~And()
{
}

void ListConcatenate::dump(int level) const
{
    Node::dump(level);
    for (auto& element : m_list)
        element->dump(level + 1);
}

RefPtr<Value> ListConcatenate::run(RefPtr<Shell> shell)
{
    RefPtr<Value> result = nullptr;

    for (auto& element : m_list) {
        if (!result) {
            result = create<ListValue>({ element->run(shell)->resolve_without_cast(shell) });
            continue;
        }
        auto element_value = element->run(shell)->resolve_without_cast(shell);

        if (result->is_command() || element_value->is_command()) {
            auto joined_commands = join_commands(result->resolve_as_commands(shell), element_value->resolve_as_commands(shell));

            if (joined_commands.size() == 1)
                result = create<CommandValue>(joined_commands[0]);
            else
                result = create<CommandSequenceValue>(move(joined_commands));
        } else {
            NonnullRefPtrVector<Value> values;

            if (result->is_list_without_resolution()) {
                values.append(static_cast<ListValue*>(result.ptr())->values());
            } else {
                for (auto& result : result->resolve_as_list(shell))
                    values.append(create<StringValue>(result));
            }

            values.append(element_value);

            result = create<ListValue>(move(values));
        }
    }
    if (!result)
        return create<ListValue>({});

    return result;
}

void ListConcatenate::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    auto first = metadata.is_first_in_list;
    metadata.is_first_in_list = false;

    metadata.is_first_in_list = first;
    for (auto& element : m_list) {
        element->highlight_in_editor(editor, shell, metadata);
        metadata.is_first_in_list = false;
    }
}

HitTestResult ListConcatenate::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    bool first = true;
    for (auto& element : m_list) {
        auto result = element->hit_test_position(offset);
        if (!result.closest_node_with_semantic_meaning && !first)
            result.closest_node_with_semantic_meaning = this;
        if (result.matching_node)
            return result;
        first = false;
    }

    return {};
}

RefPtr<Node> ListConcatenate::leftmost_trivial_literal() const
{
    if (m_list.is_empty())
        return nullptr;

    return m_list.first()->leftmost_trivial_literal();
}

ListConcatenate::ListConcatenate(Position position, Vector<RefPtr<Node>> list)
    : Node(move(position))
    , m_list(move(list))
{
    for (auto& element : m_list) {
        if (element->is_syntax_error()) {
            set_is_syntax_error(element->syntax_error_node());
            break;
        }
    }
}

ListConcatenate::~ListConcatenate()
{
}

void Background::dump(int level) const
{
    Node::dump(level);
    m_command->dump(level + 1);
}

RefPtr<Value> Background::run(RefPtr<Shell> shell)
{
    auto commands = m_command->to_lazy_evaluated_commands(shell);
    for (auto& command : commands)
        command.should_wait = false;

    return create<CommandSequenceValue>(move(commands));
}

void Background::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_command->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Background::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return m_command->hit_test_position(offset);
}

Background::Background(Position position, RefPtr<Node> command)
    : Node(move(position))
    , m_command(move(command))
{
    if (m_command->is_syntax_error())
        set_is_syntax_error(m_command->syntax_error_node());
}

Background::~Background()
{
}

void BarewordLiteral::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> BarewordLiteral::run(RefPtr<Shell>)
{
    return create<StringValue>(m_text);
}

void BarewordLiteral::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    if (metadata.is_first_in_list) {
        if (shell.is_runnable(m_text)) {
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Bold });
        } else {
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Red) });
        }

        return;
    }

    if (m_text.starts_with('-')) {
        if (m_text == "--") {
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Green) });
            return;
        }
        if (m_text == "-")
            return;

        if (m_text.starts_with("--")) {
            auto index = m_text.index_of("=").value_or(m_text.length() - 1) + 1;
            editor.stylize({ m_position.start_offset, m_position.start_offset + index }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan) });
        } else {
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan) });
        }
    }
    if (Core::File::exists(m_text)) {
        auto realpath = shell.resolve_path(m_text);
        auto url = URL::create_with_file_protocol(realpath);
        url.set_host(shell.hostname);
        editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Hyperlink(url.to_string()) });
    }
}

BarewordLiteral::BarewordLiteral(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

BarewordLiteral::~BarewordLiteral()
{
}

void CastToCommand::dump(int level) const
{
    Node::dump(level);
    m_inner->dump(level + 1);
}

RefPtr<Value> CastToCommand::run(RefPtr<Shell> shell)
{
    if (m_inner->is_command())
        return m_inner->run(shell);

    auto value = m_inner->run(shell)->resolve_without_cast(shell);
    if (value->is_command())
        return value;

    auto argv = value->resolve_as_list(shell);
    return create<CommandValue>(move(argv));
}

void CastToCommand::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult CastToCommand::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_inner->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    return result;
}

Vector<Line::CompletionSuggestion> CastToCommand::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node || !matching_node->is_bareword())
        return {};

    auto corrected_offset = offset - matching_node->position().start_offset;
    auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

    if (corrected_offset > node->text().length())
        return {};

    return shell.complete_program_name(node->text(), corrected_offset);
}

RefPtr<Node> CastToCommand::leftmost_trivial_literal() const
{
    return m_inner->leftmost_trivial_literal();
}

CastToCommand::CastToCommand(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

CastToCommand::~CastToCommand()
{
}

void CastToList::dump(int level) const
{
    Node::dump(level);
    if (m_inner)
        m_inner->dump(level + 1);
    else
        print_indented("(empty)", level + 1);
}

RefPtr<Value> CastToList::run(RefPtr<Shell> shell)
{
    if (!m_inner)
        return create<ListValue>({});

    auto inner_value = m_inner->run(shell)->resolve_without_cast(shell);

    if (inner_value->is_command() || inner_value->is_list())
        return inner_value;

    auto values = inner_value->resolve_as_list(shell);
    NonnullRefPtrVector<Value> cast_values;
    for (auto& value : values)
        cast_values.append(create<StringValue>(value));

    return create<ListValue>(cast_values);
}

void CastToList::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    if (m_inner)
        m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult CastToList::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    if (!m_inner)
        return {};

    return m_inner->hit_test_position(offset);
}

RefPtr<Node> CastToList::leftmost_trivial_literal() const
{
    return m_inner->leftmost_trivial_literal();
}

CastToList::CastToList(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner && m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

CastToList::~CastToList()
{
}

void CloseFdRedirection::dump(int level) const
{
    Node::dump(level);
    print_indented(String::format("%d -> Close", m_fd), level);
}

RefPtr<Value> CloseFdRedirection::run(RefPtr<Shell>)
{
    Command command;
    command.redirections.append(adopt(*new CloseRedirection(m_fd)));
    return create<CommandValue>(move(command));
}

void CloseFdRedirection::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset - 1 }, { Line::Style::Foreground(0x87, 0x9b, 0xcd) }); // 25% Darkened Periwinkle
    editor.stylize({ m_position.end_offset - 1, m_position.end_offset }, { Line::Style::Foreground(0xff, 0x7e, 0x00) });   // Amber
}

CloseFdRedirection::CloseFdRedirection(Position position, int fd)
    : Node(move(position))
    , m_fd(fd)
{
}

CloseFdRedirection::~CloseFdRedirection()
{
}

void CommandLiteral::dump(int level) const
{
    Node::dump(level);
    print_indented("(Generated command literal)", level + 1);
}

RefPtr<Value> CommandLiteral::run(RefPtr<Shell>)
{
    return create<CommandValue>(m_command);
}

CommandLiteral::CommandLiteral(Position position, Command command)
    : Node(move(position))
    , m_command(move(command))
{
}

CommandLiteral::~CommandLiteral()
{
}

void Comment::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> Comment::run(RefPtr<Shell>)
{
    return create<ListValue>({});
}

void Comment::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(150, 150, 150) }); // Light gray
}

Comment::Comment(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

Comment::~Comment()
{
}

void DoubleQuotedString::dump(int level) const
{
    Node::dump(level);
    m_inner->dump(level + 1);
}

RefPtr<Value> DoubleQuotedString::run(RefPtr<Shell> shell)
{
    StringBuilder builder;
    auto values = m_inner->run(shell)->resolve_as_list(shell);

    builder.join("", values);

    return create<StringValue>(builder.to_string());
}

void DoubleQuotedString::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(Line::Style::XtermColor::Yellow) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });

    editor.stylize({ m_position.start_offset, m_position.end_offset }, style);
    metadata.is_first_in_list = false;
    m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult DoubleQuotedString::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return m_inner->hit_test_position(offset);
}

DoubleQuotedString::DoubleQuotedString(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

DoubleQuotedString::~DoubleQuotedString()
{
}

void DynamicEvaluate::dump(int level) const
{
    Node::dump(level);
    m_inner->dump(level + 1);
}

RefPtr<Value> DynamicEvaluate::run(RefPtr<Shell> shell)
{
    auto result = m_inner->run(shell)->resolve_without_cast(shell);
    // Dynamic Evaluation behaves differently between strings and lists.
    // Strings are treated as variables, and Lists are treated as commands.
    if (result->is_string()) {
        auto name_part = result->resolve_as_list(shell);
        ASSERT(name_part.size() == 1);
        return create<SimpleVariableValue>(name_part[0]);
    }

    // If it's anything else, we're just gonna cast it to a list.
    auto list = result->resolve_as_list(shell);
    return create<CommandValue>(move(list));
}

void DynamicEvaluate::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });
    m_inner->highlight_in_editor(editor, shell, metadata);
}

HitTestResult DynamicEvaluate::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return m_inner->hit_test_position(offset);
}

DynamicEvaluate::DynamicEvaluate(Position position, RefPtr<Node> inner)
    : Node(move(position))
    , m_inner(move(inner))
{
    if (m_inner->is_syntax_error())
        set_is_syntax_error(m_inner->syntax_error_node());
}

DynamicEvaluate::~DynamicEvaluate()
{
}

void Fd2FdRedirection::dump(int level) const
{
    Node::dump(level);
    print_indented(String::format("%d -> %d", source_fd, dest_fd), level);
}

RefPtr<Value> Fd2FdRedirection::run(RefPtr<Shell>)
{
    Command command;
    command.redirections.append(FdRedirection::create(source_fd, dest_fd, Rewiring::Close::None));
    return create<CommandValue>(move(command));
}

void Fd2FdRedirection::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(0x87, 0x9b, 0xcd) }); // 25% Darkened Periwinkle
}

Fd2FdRedirection::Fd2FdRedirection(Position position, int src, int dst)
    : Node(move(position))
    , source_fd(src)
    , dest_fd(dst)
{
}

Fd2FdRedirection::~Fd2FdRedirection()
{
}

void ForLoop::dump(int level) const
{
    Node::dump(level);
    print_indented(String::format("%s in\n", m_variable_name.characters()), level + 1);
    m_iterated_expression->dump(level + 2);
    print_indented("Running", level + 1);
    if (m_block)
        m_block->dump(level + 2);
    else
        print_indented("(null)", level + 2);
}

RefPtr<Value> ForLoop::run(RefPtr<Shell> shell)
{
    if (!m_block)
        return create<ListValue>({});

    size_t consecutive_interruptions = 0;

    m_iterated_expression->for_each_entry(shell, [&](auto value) {
        if (consecutive_interruptions == 2)
            return IterationDecision::Break;

        RefPtr<Value> block_value;

        {
            auto frame = shell->push_frame();
            shell->set_local_variable(m_variable_name, value);

            block_value = m_block->run(shell);
        }

        if (block_value->is_job()) {
            auto job = static_cast<JobValue*>(block_value.ptr())->job();
            if (!job || job->is_running_in_background())
                return IterationDecision::Continue;
            shell->block_on_job(job);

            if (job->signaled()) {
                if (job->termination_signal() == SIGINT)
                    ++consecutive_interruptions;
                else
                    return IterationDecision::Break;
            } else {
                consecutive_interruptions = 0;
            }
        }
        return IterationDecision::Continue;
    });

    return create<ListValue>({});
}

void ForLoop::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    editor.stylize({ m_position.start_offset, m_position.start_offset + 3 }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });
    if (m_in_kw_position.has_value())
        editor.stylize({ m_in_kw_position.value(), m_in_kw_position.value() + 2 }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });

    metadata.is_first_in_list = false;
    m_iterated_expression->highlight_in_editor(editor, shell, metadata);

    metadata.is_first_in_list = true;
    if (m_block)
        m_block->highlight_in_editor(editor, shell, metadata);
}

HitTestResult ForLoop::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    if (auto result = m_iterated_expression->hit_test_position(offset); result.matching_node)
        return result;

    return m_block->hit_test_position(offset);
}

ForLoop::ForLoop(Position position, String variable_name, RefPtr<AST::Node> iterated_expr, RefPtr<AST::Node> block, Optional<size_t> in_kw_position)
    : Node(move(position))
    , m_variable_name(move(variable_name))
    , m_iterated_expression(move(iterated_expr))
    , m_block(move(block))
    , m_in_kw_position(move(in_kw_position))
{
    if (m_iterated_expression->is_syntax_error())
        set_is_syntax_error(m_iterated_expression->syntax_error_node());
    else if (m_block && m_block->is_syntax_error())
        set_is_syntax_error(m_block->syntax_error_node());
}

ForLoop::~ForLoop()
{
}

void Glob::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> Glob::run(RefPtr<Shell>)
{
    return create<GlobValue>(m_text);
}

void Glob::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(Line::Style::XtermColor::Cyan) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });
    editor.stylize({ m_position.start_offset, m_position.end_offset }, move(style));
}

Glob::Glob(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

Glob::~Glob()
{
}

void Execute::dump(int level) const
{
    Node::dump(level);
    if (m_capture_stdout)
        print_indented("(Capturing stdout)", level + 1);
    m_command->dump(level + 1);
}

void Execute::for_each_entry(RefPtr<Shell> shell, Function<IterationDecision(RefPtr<Value>)> callback)
{
    if (m_command->would_execute())
        return m_command->for_each_entry(shell, move(callback));

    auto commands = shell->expand_aliases(m_command->run(shell)->resolve_as_commands(shell));

    if (m_capture_stdout) {
        int pipefd[2];
        int rc = pipe(pipefd);
        if (rc < 0) {
            dbg() << "Error: cannot pipe(): " << strerror(errno);
            return;
        }
        auto& last_in_commands = commands.last();

        last_in_commands.redirections.prepend(FdRedirection::create(STDOUT_FILENO, pipefd[1], Rewiring::Close::Destination));
        last_in_commands.should_wait = false;
        last_in_commands.should_notify_if_in_background = false;
        last_in_commands.is_pipe_source = false;

        Core::EventLoop loop;

        auto notifier = Core::Notifier::construct(pipefd[0], Core::Notifier::Read);
        DuplexMemoryStream stream;

        enum {
            Continue,
            Break,
            NothingLeft,
        };
        auto check_and_call = [&] {
            auto ifs = shell->local_variable_or("IFS", "\n");

            if (auto offset = stream.offset_of(ifs.bytes()); offset.has_value()) {
                auto line_end = offset.value();
                if (line_end == 0) {
                    auto rc = stream.discard_or_error(ifs.length());
                    ASSERT(rc);

                    if (shell->options.inline_exec_keep_empty_segments)
                        if (callback(create<StringValue>("")) == IterationDecision::Break) {
                            loop.quit(Break);
                            notifier->set_enabled(false);
                            return Break;
                        }
                } else {
                    auto entry = ByteBuffer::create_uninitialized(line_end + ifs.length());
                    auto rc = stream.read_or_error(entry);
                    ASSERT(rc);

                    auto str = StringView(entry.data(), entry.size() - ifs.length());
                    if (callback(create<StringValue>(str)) == IterationDecision::Break) {
                        loop.quit(Break);
                        notifier->set_enabled(false);
                        return Break;
                    }
                }

                return Continue;
            }

            return NothingLeft;
        };

        notifier->on_ready_to_read = [&] {
            constexpr static auto buffer_size = 16;
            u8 buffer[buffer_size];
            size_t remaining_size = buffer_size;

            for (;;) {
                notifier->set_event_mask(Core::Notifier::None);
                bool should_enable_notifier = false;

                ScopeGuard notifier_enabler { [&] {
                    if (should_enable_notifier)
                        notifier->set_event_mask(Core::Notifier::Read);
                } };

                if (check_and_call() == Break)
                    return;

                auto read_size = read(pipefd[0], buffer, remaining_size);
                if (read_size < 0) {
                    int saved_errno = errno;
                    if (saved_errno == EINTR) {
                        should_enable_notifier = true;
                        continue;
                    }
                    if (saved_errno == 0)
                        continue;
                    dbg() << "read() failed: " << strerror(saved_errno);
                    break;
                }
                if (read_size == 0)
                    break;

                should_enable_notifier = true;
                stream.write({ buffer, (size_t)read_size });
            }

            loop.quit(Break);
        };

        shell->run_commands(commands);

        loop.exec();

        notifier->on_ready_to_read = nullptr;

        if (close(pipefd[0]) < 0) {
            dbg() << "close() failed: " << strerror(errno);
        }

        if (!stream.eof()) {
            auto action = Continue;
            do {
                action = check_and_call();
                if (action == Break)
                    return;
            } while (action == Continue);

            if (!stream.eof()) {
                auto entry = ByteBuffer::create_uninitialized(stream.remaining());
                auto rc = stream.read_or_error(entry);
                ASSERT(rc);
                callback(create<StringValue>(String::copy(entry)));
            }
        }

        return;
    }

    auto jobs = shell->run_commands(commands);

    if (!jobs.is_empty())
        callback(create<JobValue>(&jobs.last()));
}

RefPtr<Value> Execute::run(RefPtr<Shell> shell)
{
    if (m_command->would_execute())
        return m_command->run(shell);

    NonnullRefPtrVector<Value> values;
    for_each_entry(shell, [&](auto value) {
        values.append(*value);
        return IterationDecision::Continue;
    });

    if (values.size() == 1 && values.first().is_job())
        return values.first();

    return create<ListValue>(move(values));
}

void Execute::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    if (m_capture_stdout)
        editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Green) });
    metadata.is_first_in_list = true;
    m_command->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Execute::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_command->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    if (!result.closest_command_node)
        result.closest_command_node = m_command;
    return result;
}

Vector<Line::CompletionSuggestion> Execute::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node || !matching_node->is_bareword())
        return {};

    auto corrected_offset = offset - matching_node->position().start_offset;
    auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

    if (corrected_offset > node->text().length())
        return {};

    return shell.complete_program_name(node->text(), corrected_offset);
}

Execute::Execute(Position position, RefPtr<Node> command, bool capture_stdout)
    : Node(move(position))
    , m_command(move(command))
    , m_capture_stdout(capture_stdout)
{
    if (m_command->is_syntax_error())
        set_is_syntax_error(m_command->syntax_error_node());
}

Execute::~Execute()
{
}

void IfCond::dump(int level) const
{
    Node::dump(level);
    print_indented("Condition", ++level);
    m_condition->dump(level + 1);
    print_indented("True Branch", level);
    if (m_true_branch)
        m_true_branch->dump(level + 1);
    else
        print_indented("(empty)", level + 1);
    print_indented("False Branch", level);
    if (m_false_branch)
        m_false_branch->dump(level + 1);
    else
        print_indented("(empty)", level + 1);
}

RefPtr<Value> IfCond::run(RefPtr<Shell> shell)
{
    auto cond = m_condition->run(shell)->resolve_without_cast(shell);
    ASSERT(cond->is_job());

    auto cond_job_value = static_cast<const JobValue*>(cond.ptr());
    auto cond_job = cond_job_value->job();

    shell->block_on_job(cond_job);

    if (cond_job->signaled())
        return create<ListValue>({}); // Exit early.

    if (shell->last_return_code == 0) {
        if (m_true_branch)
            return m_true_branch->run(shell);
    } else {
        if (m_false_branch)
            return m_false_branch->run(shell);
    }

    return create<ListValue>({});
}

void IfCond::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    metadata.is_first_in_list = true;

    editor.stylize({ m_position.start_offset, m_position.start_offset + 2 }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });
    if (m_else_position.has_value())
        editor.stylize({ m_else_position.value().start_offset, m_else_position.value().start_offset + 4 }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });

    m_condition->highlight_in_editor(editor, shell, metadata);
    if (m_true_branch)
        m_true_branch->highlight_in_editor(editor, shell, metadata);
    if (m_false_branch)
        m_false_branch->highlight_in_editor(editor, shell, metadata);
}

HitTestResult IfCond::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    if (auto result = m_condition->hit_test_position(offset); result.matching_node)
        return result;

    if (m_true_branch) {
        if (auto result = m_true_branch->hit_test_position(offset); result.matching_node)
            return result;
    }

    if (m_false_branch) {
        if (auto result = m_false_branch->hit_test_position(offset); result.matching_node)
            return result;
    }

    return {};
}

IfCond::IfCond(Position position, Optional<Position> else_position, RefPtr<Node> condition, RefPtr<Node> true_branch, RefPtr<Node> false_branch)
    : Node(move(position))
    , m_condition(move(condition))
    , m_true_branch(move(true_branch))
    , m_false_branch(move(false_branch))
    , m_else_position(move(else_position))
{
    if (m_condition->is_syntax_error())
        set_is_syntax_error(m_condition->syntax_error_node());
    else if (m_true_branch && m_true_branch->is_syntax_error())
        set_is_syntax_error(m_true_branch->syntax_error_node());
    else if (m_false_branch && m_false_branch->is_syntax_error())
        set_is_syntax_error(m_false_branch->syntax_error_node());

    m_condition = create<AST::Execute>(m_condition->position(), m_condition);
    if (m_true_branch)
        m_true_branch = create<AST::Execute>(m_true_branch->position(), m_true_branch);
    if (m_false_branch)
        m_false_branch = create<AST::Execute>(m_false_branch->position(), m_false_branch);
}

IfCond::~IfCond()
{
}

void Join::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Join::run(RefPtr<Shell> shell)
{
    auto left = m_left->to_lazy_evaluated_commands(shell);
    auto right = m_right->to_lazy_evaluated_commands(shell);

    return create<CommandSequenceValue>(join_commands(move(left), move(right)));
}

void Join::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    if (m_left->is_list() || m_left->is_command())
        metadata.is_first_in_list = false;
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Join::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

RefPtr<Node> Join::leftmost_trivial_literal() const
{
    if (auto value = m_left->leftmost_trivial_literal())
        return value;
    return m_right->leftmost_trivial_literal();
}

Join::Join(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Join::~Join()
{
}

void Or::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Or::run(RefPtr<Shell> shell)
{
    auto commands = m_left->to_lazy_evaluated_commands(shell);
    commands.last().next_chain.empend(*m_right, NodeWithAction::Or);
    return create<CommandSequenceValue>(move(commands));
}

void Or::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Or::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node) {
        if (!result.closest_command_node)
            result.closest_command_node = m_right;
        return result;
    }
    result = m_right->hit_test_position(offset);
    if (!result.closest_command_node)
        result.closest_command_node = m_right;
    return result;
}

Or::Or(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Or::~Or()
{
}

void Pipe::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Pipe::run(RefPtr<Shell> shell)
{
    auto left = m_left->to_lazy_evaluated_commands(shell);
    auto right = m_right->to_lazy_evaluated_commands(shell);

    auto last_in_left = left.take_last();
    auto first_in_right = right.take_first();

    auto pipe_read_end = FdRedirection::create(STDIN_FILENO, -1, Rewiring::Close::Destination);
    auto pipe_write_end = FdRedirection::create(STDOUT_FILENO, -1, pipe_read_end, Rewiring::Close::RefreshDestination);
    first_in_right.redirections.append(pipe_read_end);
    last_in_left.redirections.append(pipe_write_end);
    last_in_left.should_wait = false;
    last_in_left.is_pipe_source = true;

    if (first_in_right.pipeline) {
        last_in_left.pipeline = first_in_right.pipeline;
    } else {
        auto pipeline = adopt(*new Pipeline);
        last_in_left.pipeline = pipeline;
        first_in_right.pipeline = pipeline;
    }

    Vector<Command> commands;
    commands.append(left);
    commands.append(last_in_left);
    commands.append(first_in_right);
    commands.append(right);

    return create<CommandSequenceValue>(move(commands));
}

void Pipe::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Pipe::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

Pipe::Pipe(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Pipe::~Pipe()
{
}

PathRedirectionNode::PathRedirectionNode(Position position, int fd, RefPtr<Node> path)
    : Node(move(position))
    , m_fd(fd)
    , m_path(move(path))
{
}

void PathRedirectionNode::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(0x87, 0x9b, 0xcd) }); // 25% Darkened Periwinkle
    metadata.is_first_in_list = false;
    m_path->highlight_in_editor(editor, shell, metadata);
    if (m_path->is_bareword()) {
        auto path_text = m_path->run(nullptr)->resolve_as_list(nullptr);
        ASSERT(path_text.size() == 1);
        // Apply a URL to the path.
        auto& position = m_path->position();
        auto& path = path_text[0];
        if (!path.starts_with('/'))
            path = String::format("%s/%s", shell.cwd.characters(), path.characters());
        auto url = URL::create_with_file_protocol(path);
        url.set_host(shell.hostname);
        editor.stylize({ position.start_offset, position.end_offset }, { Line::Style::Hyperlink(url.to_string()) });
    }
}

HitTestResult PathRedirectionNode::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_path->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    return result;
}

Vector<Line::CompletionSuggestion> PathRedirectionNode::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node || !matching_node->is_bareword())
        return {};

    auto corrected_offset = offset - matching_node->position().start_offset;
    auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

    if (corrected_offset > node->text().length())
        return {};

    return shell.complete_path("", node->text(), corrected_offset);
}

PathRedirectionNode::~PathRedirectionNode()
{
}

void ReadRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("To %d", m_fd), level + 1);
}

RefPtr<Value> ReadRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(PathRedirection::create(builder.to_string(), m_fd, PathRedirection::Read));
    return create<CommandValue>(move(command));
}

ReadRedirection::ReadRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

ReadRedirection::~ReadRedirection()
{
}

void ReadWriteRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("To/From %d", m_fd), level + 1);
}

RefPtr<Value> ReadWriteRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(PathRedirection::create(builder.to_string(), m_fd, PathRedirection::ReadWrite));
    return create<CommandValue>(move(command));
}

ReadWriteRedirection::ReadWriteRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

ReadWriteRedirection::~ReadWriteRedirection()
{
}

void Sequence::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Sequence::run(RefPtr<Shell> shell)
{
    // If we are to return a job, block on the left one then return the right one.
    if (would_execute()) {
        RefPtr<AST::Node> execute_node = create<AST::Execute>(m_left->position(), m_left);
        auto left_value = execute_node->run(shell);
        // Some nodes are inherently empty, such as Comments and For loops without bodies,
        // it is not an error for the value not to be a job.
        if (left_value && left_value->is_job())
            shell->block_on_job(static_cast<JobValue*>(left_value.ptr())->job());

        if (m_right->would_execute())
            return m_right->run(shell);

        execute_node = create<AST::Execute>(m_right->position(), m_right);
        return execute_node->run(shell);
    }

    auto left = m_left->to_lazy_evaluated_commands(shell);
    // This could happen if a comment is next to a command.
    if (left.size() == 1) {
        auto& command = left.first();
        if (command.argv.is_empty() && command.redirections.is_empty())
            return m_right->run(shell);
    }

    if (left.last().should_wait)
        left.last().next_chain.append(NodeWithAction { *m_right, NodeWithAction::Sequence });
    else
        left.append(m_right->to_lazy_evaluated_commands(shell));

    return create<CommandSequenceValue>(move(left));
}

void Sequence::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Sequence::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

Sequence::Sequence(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Sequence::~Sequence()
{
}

void Subshell::dump(int level) const
{
    Node::dump(level);
    if (m_block)
        m_block->dump(level + 1);
}

RefPtr<Value> Subshell::run(RefPtr<Shell> shell)
{
    if (!m_block)
        return create<ListValue>({});

    return m_block->run(shell);
}

void Subshell::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    metadata.is_first_in_list = true;
    if (m_block)
        m_block->highlight_in_editor(editor, shell, metadata);
}

HitTestResult Subshell::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    if (m_block)
        return m_block->hit_test_position(offset);

    return {};
}

Subshell::Subshell(Position position, RefPtr<Node> block)
    : Node(move(position))
    , m_block(block)
{
    if (m_block && m_block->is_syntax_error())
        set_is_syntax_error(m_block->syntax_error_node());
}

Subshell::~Subshell()
{
}

void SimpleVariable::dump(int level) const
{
    Node::dump(level);
    print_indented(m_name, level + 1);
}

RefPtr<Value> SimpleVariable::run(RefPtr<Shell>)
{
    return create<SimpleVariableValue>(m_name);
}

void SimpleVariable::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(214, 112, 214) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });
    editor.stylize({ m_position.start_offset, m_position.end_offset }, move(style));
}

HitTestResult SimpleVariable::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return { this, this, nullptr };
}

Vector<Line::CompletionSuggestion> SimpleVariable::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node)
        return {};

    if (matching_node != this)
        return {};

    auto corrected_offset = offset - matching_node->position().start_offset - 1;

    if (corrected_offset > m_name.length() + 1)
        return {};

    return shell.complete_variable(m_name, corrected_offset);
}

SimpleVariable::SimpleVariable(Position position, String name)
    : Node(move(position))
    , m_name(move(name))
{
}

SimpleVariable::~SimpleVariable()
{
}

void SpecialVariable::dump(int level) const
{
    Node::dump(level);
    print_indented(String { &m_name, 1 }, level + 1);
}

RefPtr<Value> SpecialVariable::run(RefPtr<Shell>)
{
    return create<SpecialVariableValue>(m_name);
}

void SpecialVariable::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(214, 112, 214) });
}

Vector<Line::CompletionSuggestion> SpecialVariable::complete_for_editor(Shell&, size_t, const HitTestResult&)
{
    return {};
}

HitTestResult SpecialVariable::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return { this, this, nullptr };
}

SpecialVariable::SpecialVariable(Position position, char name)
    : Node(move(position))
    , m_name(name)
{
}

SpecialVariable::~SpecialVariable()
{
}

void Juxtaposition::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> Juxtaposition::run(RefPtr<Shell> shell)
{
    auto left_value = m_left->run(shell)->resolve_without_cast(shell);
    auto right_value = m_right->run(shell)->resolve_without_cast(shell);

    auto left = left_value->resolve_as_list(shell);
    auto right = right_value->resolve_as_list(shell);

    if (left_value->is_string() && right_value->is_string()) {

        ASSERT(left.size() == 1);
        ASSERT(right.size() == 1);

        StringBuilder builder;
        builder.append(left[0]);
        builder.append(right[0]);

        return create<StringValue>(builder.to_string());
    }

    // Otherwise, treat them as lists and create a list product.
    if (left.is_empty() || right.is_empty())
        return create<ListValue>({});

    Vector<String> result;
    result.ensure_capacity(left.size() * right.size());

    StringBuilder builder;
    for (auto& left_element : left) {
        for (auto& right_element : right) {
            builder.append(left_element);
            builder.append(right_element);
            result.append(builder.to_string());
            builder.clear();
        }
    }

    return create<ListValue>(move(result));
}

void Juxtaposition::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);

    // '~/foo/bar' is special, we have to actually resolve the tilde
    // since that resolution is a pure operation, we can just go ahead
    // and do it to get the value :)
    if (m_right->is_bareword() && m_left->is_tilde()) {
        auto tilde_value = m_left->run(shell)->resolve_as_list(shell)[0];
        auto bareword_value = m_right->run(shell)->resolve_as_list(shell)[0];

        StringBuilder path_builder;
        path_builder.append(tilde_value);
        path_builder.append("/");
        path_builder.append(bareword_value);
        auto path = path_builder.to_string();

        if (Core::File::exists(path)) {
            auto realpath = shell.resolve_path(path);
            auto url = URL::create_with_file_protocol(realpath);
            url.set_host(shell.hostname);
            editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Hyperlink(url.to_string()) });
        }

    } else {
        m_right->highlight_in_editor(editor, shell, metadata);
    }
}

Vector<Line::CompletionSuggestion> Juxtaposition::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    // '~/foo/bar' is special, we have to actually resolve the tilde
    // then complete the bareword with that path prefix.
    if (m_right->is_bareword() && m_left->is_tilde()) {
        auto tilde_value = m_left->run(shell)->resolve_as_list(shell)[0];

        auto corrected_offset = offset - matching_node->position().start_offset;
        auto* node = static_cast<BarewordLiteral*>(matching_node.ptr());

        if (corrected_offset > node->text().length())
            return {};

        auto text = node->text().substring(1, node->text().length() - 1);

        return shell.complete_path(tilde_value, text, corrected_offset - 1);
    }

    return Node::complete_for_editor(shell, offset, hit_test_result);
}

HitTestResult Juxtaposition::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    if (result.matching_node)
        return result;

    result = m_right->hit_test_position(offset);
    if (!result.closest_node_with_semantic_meaning)
        result.closest_node_with_semantic_meaning = this;
    return result;
}

Juxtaposition::Juxtaposition(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

Juxtaposition::~Juxtaposition()
{
}

void StringLiteral::dump(int level) const
{
    Node::dump(level);
    print_indented(m_text, level + 1);
}

RefPtr<Value> StringLiteral::run(RefPtr<Shell>)
{
    return create<StringValue>(m_text);
}

void StringLiteral::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata metadata)
{
    Line::Style style { Line::Style::Foreground(Line::Style::XtermColor::Yellow) };
    if (metadata.is_first_in_list)
        style.unify_with({ Line::Style::Bold });
    editor.stylize({ m_position.start_offset, m_position.end_offset }, move(style));
}

StringLiteral::StringLiteral(Position position, String text)
    : Node(move(position))
    , m_text(move(text))
{
}

StringLiteral::~StringLiteral()
{
}

void StringPartCompose::dump(int level) const
{
    Node::dump(level);
    m_left->dump(level + 1);
    m_right->dump(level + 1);
}

RefPtr<Value> StringPartCompose::run(RefPtr<Shell> shell)
{
    auto left = m_left->run(shell)->resolve_as_list(shell);
    auto right = m_right->run(shell)->resolve_as_list(shell);

    StringBuilder builder;
    builder.join(" ", left);
    builder.join(" ", right);

    return create<StringValue>(builder.to_string());
}

void StringPartCompose::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    m_left->highlight_in_editor(editor, shell, metadata);
    m_right->highlight_in_editor(editor, shell, metadata);
}

HitTestResult StringPartCompose::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    auto result = m_left->hit_test_position(offset);
    if (result.matching_node)
        return result;
    return m_right->hit_test_position(offset);
}

StringPartCompose::StringPartCompose(Position position, RefPtr<Node> left, RefPtr<Node> right)
    : Node(move(position))
    , m_left(move(left))
    , m_right(move(right))
{
    if (m_left->is_syntax_error())
        set_is_syntax_error(m_left->syntax_error_node());
    else if (m_right->is_syntax_error())
        set_is_syntax_error(m_right->syntax_error_node());
}

StringPartCompose::~StringPartCompose()
{
}

void SyntaxError::dump(int level) const
{
    Node::dump(level);
}

RefPtr<Value> SyntaxError::run(RefPtr<Shell>)
{
    dbg() << "SYNTAX ERROR AAAA";
    return create<StringValue>("");
}

void SyntaxError::highlight_in_editor(Line::Editor& editor, Shell&, HighlightMetadata)
{
    editor.stylize({ m_position.start_offset, m_position.end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Red), Line::Style::Bold });
}

SyntaxError::SyntaxError(Position position, String error)
    : Node(move(position))
    , m_syntax_error_text(move(error))
{
    m_is_syntax_error = true;
}

const SyntaxError& SyntaxError::syntax_error_node() const
{
    return *this;
}

SyntaxError::~SyntaxError()
{
}

void Tilde::dump(int level) const
{
    Node::dump(level);
    print_indented(m_username, level + 1);
}

RefPtr<Value> Tilde::run(RefPtr<Shell>)
{
    return create<TildeValue>(m_username);
}

void Tilde::highlight_in_editor(Line::Editor&, Shell&, HighlightMetadata)
{
}

HitTestResult Tilde::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    return { this, this, nullptr };
}

Vector<Line::CompletionSuggestion> Tilde::complete_for_editor(Shell& shell, size_t offset, const HitTestResult& hit_test_result)
{
    auto matching_node = hit_test_result.matching_node;
    if (!matching_node)
        return {};

    if (matching_node != this)
        return {};

    auto corrected_offset = offset - matching_node->position().start_offset - 1;

    if (corrected_offset > m_username.length() + 1)
        return {};

    return shell.complete_user(m_username, corrected_offset);
}

String Tilde::text() const
{
    StringBuilder builder;
    builder.append('~');
    builder.append(m_username);
    return builder.to_string();
}

Tilde::Tilde(Position position, String username)
    : Node(move(position))
    , m_username(move(username))
{
}

Tilde::~Tilde()
{
}

void WriteAppendRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("From %d", m_fd), level + 1);
}

RefPtr<Value> WriteAppendRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(PathRedirection::create(builder.to_string(), m_fd, PathRedirection::WriteAppend));
    return create<CommandValue>(move(command));
}

WriteAppendRedirection::WriteAppendRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

WriteAppendRedirection::~WriteAppendRedirection()
{
}

void WriteRedirection::dump(int level) const
{
    Node::dump(level);
    m_path->dump(level + 1);
    print_indented(String::format("From %d", m_fd), level + 1);
}

RefPtr<Value> WriteRedirection::run(RefPtr<Shell> shell)
{
    Command command;
    auto path_segments = m_path->run(shell)->resolve_as_list(shell);
    StringBuilder builder;
    builder.join(" ", path_segments);

    command.redirections.append(PathRedirection::create(builder.to_string(), m_fd, PathRedirection::Write));
    return create<CommandValue>(move(command));
}

WriteRedirection::WriteRedirection(Position position, int fd, RefPtr<Node> path)
    : PathRedirectionNode(move(position), fd, move(path))
{
}

WriteRedirection::~WriteRedirection()
{
}

void VariableDeclarations::dump(int level) const
{
    Node::dump(level);
    for (auto& var : m_variables) {
        print_indented("Set", level + 1);
        var.name->dump(level + 2);
        var.value->dump(level + 2);
    }
}

RefPtr<Value> VariableDeclarations::run(RefPtr<Shell> shell)
{
    for (auto& var : m_variables) {
        auto name_value = var.name->run(shell)->resolve_as_list(shell);
        ASSERT(name_value.size() == 1);
        auto name = name_value[0];
        auto value = var.value->run(shell);
        if (value->is_list()) {
            auto parts = value->resolve_as_list(shell);
            shell->set_local_variable(name, adopt(*new ListValue(move(parts))));
        } else if (value->is_command()) {
            shell->set_local_variable(name, value);
        } else {
            auto part = value->resolve_as_list(shell);
            shell->set_local_variable(name, adopt(*new StringValue(part[0])));
        }
    }

    return create<ListValue>({});
}

void VariableDeclarations::highlight_in_editor(Line::Editor& editor, Shell& shell, HighlightMetadata metadata)
{
    metadata.is_first_in_list = false;
    for (auto& var : m_variables) {
        var.name->highlight_in_editor(editor, shell, metadata);
        // Highlight the '='.
        editor.stylize({ var.name->position().end_offset - 1, var.name->position().end_offset }, { Line::Style::Foreground(Line::Style::XtermColor::Blue) });
        var.value->highlight_in_editor(editor, shell, metadata);
    }
}

HitTestResult VariableDeclarations::hit_test_position(size_t offset)
{
    if (!position().contains(offset))
        return {};

    for (auto decl : m_variables) {
        auto result = decl.value->hit_test_position(offset);
        if (result.matching_node)
            return result;
    }

    return { nullptr, nullptr, nullptr };
}

VariableDeclarations::VariableDeclarations(Position position, Vector<Variable> variables)
    : Node(move(position))
    , m_variables(move(variables))
{
    for (auto& decl : m_variables) {
        if (decl.name->is_syntax_error()) {
            set_is_syntax_error(decl.name->syntax_error_node());
            break;
        }
        if (decl.value->is_syntax_error()) {
            set_is_syntax_error(decl.value->syntax_error_node());
            break;
        }
    }
}

VariableDeclarations::~VariableDeclarations()
{
}

Value::~Value()
{
}
Vector<AST::Command> Value::resolve_as_commands(RefPtr<Shell> shell)
{
    Command command;
    command.argv = resolve_as_list(shell);
    return { command };
}

ListValue::ListValue(Vector<String> values)
{
    m_contained_values.ensure_capacity(values.size());
    for (auto& str : values)
        m_contained_values.append(adopt(*new StringValue(move(str))));
}

ListValue::~ListValue()
{
}

Vector<String> ListValue::resolve_as_list(RefPtr<Shell> shell)
{
    Vector<String> values;
    for (auto& value : m_contained_values)
        values.append(value.resolve_as_list(shell));

    return values;
}

NonnullRefPtr<Value> ListValue::resolve_without_cast(RefPtr<Shell> shell)
{
    NonnullRefPtrVector<Value> values;
    for (auto& value : m_contained_values)
        values.append(value.resolve_without_cast(shell));

    return create<ListValue>(move(values));
}

CommandValue::~CommandValue()
{
}

CommandSequenceValue::~CommandSequenceValue()
{
}

Vector<String> CommandSequenceValue::resolve_as_list(RefPtr<Shell>)
{
    // TODO: Somehow raise an "error".
    return {};
}

Vector<Command> CommandSequenceValue::resolve_as_commands(RefPtr<Shell>)
{
    return m_contained_values;
}

Vector<String> CommandValue::resolve_as_list(RefPtr<Shell>)
{
    // TODO: Somehow raise an "error".
    return {};
}

Vector<Command> CommandValue::resolve_as_commands(RefPtr<Shell>)
{
    return { m_command };
}

JobValue::~JobValue()
{
}

StringValue::~StringValue()
{
}
Vector<String> StringValue::resolve_as_list(RefPtr<Shell>)
{
    if (is_list()) {
        auto parts = StringView(m_string).split_view(m_split, m_keep_empty);
        Vector<String> result;
        result.ensure_capacity(parts.size());
        for (auto& part : parts)
            result.append(part);
        return result;
    }

    return { m_string };
}

GlobValue::~GlobValue()
{
}
Vector<String> GlobValue::resolve_as_list(RefPtr<Shell> shell)
{
    return shell->expand_globs(m_glob, shell->cwd);
}

SimpleVariableValue::~SimpleVariableValue()
{
}
Vector<String> SimpleVariableValue::resolve_as_list(RefPtr<Shell> shell)
{
    if (auto value = resolve_without_cast(shell); value != this)
        return value->resolve_as_list(shell);

    char* env_value = getenv(m_name.characters());
    if (env_value == nullptr)
        return { "" };

    Vector<String> res;
    String str_env_value = String(env_value);
    const auto& split_text = str_env_value.split_view(' ');
    for (auto& part : split_text)
        res.append(part);
    return res;
}

NonnullRefPtr<Value> SimpleVariableValue::resolve_without_cast(RefPtr<Shell> shell)
{
    if (auto value = shell->lookup_local_variable(m_name))
        return value.release_nonnull();
    return *this;
}

SpecialVariableValue::~SpecialVariableValue()
{
}

Vector<String> SpecialVariableValue::resolve_as_list(RefPtr<Shell> shell)
{
    switch (m_name) {
    case '?':
        return { String::number(shell->last_return_code) };
    case '$':
        return { String::number(getpid()) };
    case '*':
        if (auto argv = shell->lookup_local_variable("ARGV"))
            return argv->resolve_as_list(shell);
        return {};
    case '#':
        if (auto argv = shell->lookup_local_variable("ARGV")) {
            if (argv->is_list()) {
                auto list_argv = static_cast<AST::ListValue*>(argv.ptr());
                return { String::number(list_argv->values().size()) };
            }
            return { "1" };
        }
        return { "0" };
    default:
        return { "" };
    }
}

TildeValue::~TildeValue()
{
}
Vector<String> TildeValue::resolve_as_list(RefPtr<Shell> shell)
{
    StringBuilder builder;
    builder.append("~");
    builder.append(m_username);
    return { shell->expand_tilde(builder.to_string()) };
}

Result<NonnullRefPtr<Rewiring>, String> CloseRedirection::apply() const
{
    return adopt(*new Rewiring(fd, fd, Rewiring::Close::ImmediatelyCloseDestination));
}

CloseRedirection::~CloseRedirection()
{
}

Result<NonnullRefPtr<Rewiring>, String> PathRedirection::apply() const
{
    auto check_fd_and_return = [my_fd = this->fd](int fd, const String& path) -> Result<NonnullRefPtr<Rewiring>, String> {
        if (fd < 0) {
            String error = strerror(errno);
            dbg() << "open() failed for '" << path << "' with " << error;
            return error;
        }
        return adopt(*new Rewiring(my_fd, fd, Rewiring::Close::Destination));
    };
    switch (direction) {
    case AST::PathRedirection::WriteAppend:
        return check_fd_and_return(open(path.characters(), O_WRONLY | O_CREAT | O_APPEND, 0666), path);

    case AST::PathRedirection::Write:
        return check_fd_and_return(open(path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666), path);

    case AST::PathRedirection::Read:
        return check_fd_and_return(open(path.characters(), O_RDONLY), path);

    case AST::PathRedirection::ReadWrite:
        return check_fd_and_return(open(path.characters(), O_RDWR | O_CREAT, 0666), path);
    }

    ASSERT_NOT_REACHED();
}

PathRedirection::~PathRedirection()
{
}

FdRedirection::~FdRedirection()
{
}

Redirection::~Redirection()
{
}

}
