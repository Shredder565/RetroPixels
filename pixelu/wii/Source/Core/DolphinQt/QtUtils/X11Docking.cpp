// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/X11Docking.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

#include <QGuiApplication>

#if defined(PIXELU_QT_X11) && QT_CONFIG(xcb)
#include <xcb/xcb.h>
#define PIXELU_X11_DOCKING 1
#endif

namespace QtUtils::X11Docking
{
#ifdef PIXELU_X11_DOCKING
namespace
{
constexpr char EMBED_PROPERTY[] = "_RETROPIXELS_EMBED";
constexpr char DOCK_PROPERTY[] = "_RETROPIXELS_DOCK";

// xcb replies are malloc'ed and freed with free().
struct FreeReply
{
  void operator()(void* reply) const { std::free(reply); }
};
template <typename T>
using Reply = std::unique_ptr<T, FreeReply>;

xcb_connection_t* Connection()
{
  if (QGuiApplication::platformName() != QLatin1String("xcb"))
    return nullptr;
  const auto* const x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  return x11 != nullptr ? x11->connection() : nullptr;
}

xcb_atom_t Atom(xcb_connection_t* connection, const char* name)
{
  const xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(connection, 0, static_cast<uint16_t>(std::strlen(name)), name);
  const Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection, cookie, nullptr));
  return reply ? reply->atom : XCB_ATOM_NONE;
}

struct TreePosition
{
  xcb_window_t root;
  xcb_window_t parent;
};

std::optional<TreePosition> WhereIs(xcb_connection_t* connection, xcb_window_t window)
{
  const Reply<xcb_query_tree_reply_t> reply(
      xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr));
  if (!reply)
    return std::nullopt;
  return TreePosition{reply->root, reply->parent};
}

// The unmapped window that holds game screens until RetroPixels takes them, one per root
// window (there is one screen in practice). It lives as long as Qt's connection.
xcb_window_t Holder(xcb_connection_t* connection, xcb_window_t root)
{
  static xcb_window_t holder = XCB_WINDOW_NONE;
  static xcb_window_t holder_root = XCB_WINDOW_NONE;
  if (holder != XCB_WINDOW_NONE && holder_root == root)
    return holder;

  const xcb_window_t id = xcb_generate_id(connection);
  const xcb_void_cookie_t cookie =
      xcb_create_window_checked(connection, XCB_COPY_FROM_PARENT, id, root, -16, -16, 1, 1, 0,
                                XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
  if (xcb_generic_error_t* const error = xcb_request_check(connection, cookie))
  {
    std::free(error);
    return XCB_WINDOW_NONE;
  }
  holder = id;
  holder_root = root;
  return holder;
}

bool Check(xcb_connection_t* connection, xcb_void_cookie_t cookie)
{
  xcb_generic_error_t* const error = xcb_request_check(connection, cookie);
  std::free(error);
  return error == nullptr;
}
}  // namespace

bool IsAvailable()
{
  return Connection() != nullptr;
}

bool Prepare(WId window)
{
  xcb_connection_t* const connection = Connection();
  if (connection == nullptr || window == 0)
    return false;
  const auto id = static_cast<xcb_window_t>(window);

  const std::optional<TreePosition> where = WhereIs(connection, id);
  if (!where)
    return false;
  const xcb_window_t holder = Holder(connection, where->root);
  const xcb_atom_t mark = Atom(connection, EMBED_PROPERTY);
  if (holder == XCB_WINDOW_NONE || mark == XCB_ATOM_NONE)
    return false;

  const uint32_t one = 1;
  if (!Check(connection, xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE, id, mark,
                                                     XCB_ATOM_CARDINAL, 32, 1, &one)))
  {
    return false;
  }
  // Already in the holder, or already docked (a second boot): nothing to move.
  if (where->parent != where->root)
    return true;
  return Check(connection, xcb_reparent_window_checked(connection, id, holder, 0, 0));
}

bool IsDocked(WId window)
{
  xcb_connection_t* const connection = Connection();
  if (connection == nullptr || window == 0)
    return false;

  const std::optional<TreePosition> where = WhereIs(connection, static_cast<xcb_window_t>(window));
  if (!where || where->parent == where->root)
    return false;

  static xcb_atom_t dock = XCB_ATOM_NONE;
  if (dock == XCB_ATOM_NONE)
    dock = Atom(connection, DOCK_PROPERTY);
  if (dock == XCB_ATOM_NONE)
    return false;

  const xcb_get_property_cookie_t cookie =
      xcb_get_property(connection, 0, where->parent, dock, XCB_ATOM_CARDINAL, 0, 1);
  const Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection, cookie, nullptr));
  return reply && reply->format == 32 && xcb_get_property_value_length(reply.get()) >= 4;
}

void Release(WId window, QPoint position)
{
  xcb_connection_t* const connection = Connection();
  if (connection == nullptr || window == 0)
    return;
  const auto id = static_cast<xcb_window_t>(window);

  xcb_delete_property(connection, id, Atom(connection, EMBED_PROPERTY));
  const std::optional<TreePosition> where = WhereIs(connection, id);
  if (where && where->parent != where->root)
  {
    // A mapped window is unmapped and mapped again by the reparent; the new map request goes
    // to the window manager, which frames it as a new window.
    xcb_reparent_window(connection, id, where->root, static_cast<int16_t>(position.x()),
                        static_cast<int16_t>(position.y()));
  }
  xcb_flush(connection);
}
#else
bool IsAvailable()
{
  return false;
}

bool Prepare(WId)
{
  return false;
}

bool IsDocked(WId)
{
  return false;
}

void Release(WId, QPoint)
{
}
#endif
}  // namespace QtUtils::X11Docking
