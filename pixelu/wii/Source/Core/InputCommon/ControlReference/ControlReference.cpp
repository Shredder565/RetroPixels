// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControlReference/ControlReference.h"

#include <utility>

namespace
{
// Open by default on every thread.
thread_local bool t_gate_open = true;
}  // namespace

void ControlReference::SetInputGate(bool enable)
{
  t_gate_open = enable;
}

bool ControlReference::GetInputGate()
{
  return t_gate_open;
}

ControlReference::ControlReference() = default;
ControlReference::~ControlReference() = default;

void ControlReference::UpdateReference(ciface::ExpressionParser::ControlEnvironment& env)
{
  if (m_parsed_expression != nullptr)
    m_parsed_expression->UpdateReferences(env);
}

int ControlReference::BoundCount() const
{
  return m_parsed_expression != nullptr ? m_parsed_expression->CountNumControls() : 0;
}

ciface::ExpressionParser::ParseStatus ControlReference::GetParseStatus() const
{
  return m_parse_status;
}

std::string ControlReference::GetExpression() const
{
  return m_expression;
}

std::optional<std::string> ControlReference::SetExpression(std::string expr)
{
  m_expression = std::move(expr);

  ciface::ExpressionParser::ParseResult parsed =
      ciface::ExpressionParser::ParseExpression(m_expression);
  m_parse_status = parsed.status;
  m_parsed_expression = std::move(parsed.expr);
  return std::move(parsed.description);
}

InputReference::InputReference() = default;
OutputReference::OutputReference() = default;

bool InputReference::IsInput() const
{
  return true;
}

bool OutputReference::IsInput() const
{
  return false;
}

ControlState InputReference::State(const ControlState)
{
  // A closed gate or a missing expression both read as released.
  if (m_parsed_expression == nullptr || !GetInputGate())
    return 0.0;
  return m_parsed_expression->GetValue() * range;
}

ControlState OutputReference::State(const ControlState state)
{
  // Outputs ignore the input gate: rumble still has to be switched off when focus is lost.
  if (m_parsed_expression != nullptr)
    m_parsed_expression->SetValue(state * range);
  return 0.0;
}
