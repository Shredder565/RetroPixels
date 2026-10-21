// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/SearchBar.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>

SearchBar::SearchBar(QWidget* parent) : QWidget(parent)
{
  m_query = new QLineEdit;
  m_close = new QPushButton(tr("Close"));
  m_query->setPlaceholderText(tr("Search games..."));

  auto* const row = new QHBoxLayout;
  row->addWidget(m_query);
  row->addWidget(m_close);
  row->setSizeConstraint(QLayout::SetMinAndMaxSize);
  setLayout(row);

  // Every edit re-filters the list straight away.
  connect(m_query, &QLineEdit::textChanged, this, &SearchBar::Search);
  connect(m_close, &QPushButton::clicked, this, &SearchBar::Hide);

  setFixedHeight(32);
  setHidden(true);

  installEventFilter(this);
}

void SearchBar::Show()
{
  m_query->setFocus();
  m_query->selectAll();

  // The text may still be there from last time; filter with it again.
  emit Search(m_query->text());

  show();
}

void SearchBar::Hide()
{
  // Hiding the bar shows every game again, but keeps the text for next time.
  emit Search(QString());

  m_query->clearFocus();

  hide();
}

bool SearchBar::eventFilter(QObject*, QEvent* event)
{
  if (event->type() == QEvent::KeyPress &&
      static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
  {
    Hide();
  }
  return false;
}
