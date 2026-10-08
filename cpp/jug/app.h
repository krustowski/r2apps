//
//  app.h --- jug's two faces: the console and the Memento window.
//
#pragma once

#include <r2/string_view.hpp>

namespace jug {

//  `jug <command> ...` on the console (cli.cpp).
int cli();

//  `jug.elf --host 0x<address>`: the contents of Memento's Jug window
//  (hosted.cpp).  `block` is the address of the window's host block.
int hosted(r2::string_view block);

} // namespace jug
