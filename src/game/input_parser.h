#ifndef RLGAMES_INPUT_PARSER
#define RLGAMES_INPUT_PARSER

#include <string>
#include <iostream>
#include <print>

#include <type_alias.h>
#include <types.h>

namespace rlgames {

namespace s = std;

Move parse_move(uint board_size) {
  s::string input;
  do {
    s::getline(s::cin, input);
    auto opt = string_to_move(input, board_size);
    if (opt) return *opt;
    s::println("unable to parse input. try again");
  } while (true);
}

}  // namespace rlgames

#endif  // RLGAMES_INPUT_PARSER
