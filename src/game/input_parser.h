#ifndef RLGAMES_INPUT_PARSER
#define RLGAMES_INPUT_PARSER

#include <string>
#include <iostream>
#include <istream>
#include <ostream>
#include <optional>

#include <type_alias.h>
#include <types.h>

namespace rlgames {

namespace s = std;

// read moves until one parses and is legal in gs;
// returns nullopt if the input stream ends first
template <typename GameState>
s::optional<Move> parse_move(
    const GameState& gs,
    uint board_size,
    s::istream& in = s::cin,
    s::ostream& out = s::cout)
{
  s::string input;
  while (s::getline(in, input)) {
    auto opt = string_to_move(input, board_size);
    if (not opt)
      out << "unable to parse input (e.g. E5, pass, resign). try again\n";
    else if (not gs.is_valid_move(*opt))
      out << "illegal move. try again\n";
    else
      return opt;
  }
  return s::nullopt;
}

}  // namespace rlgames

#endif  // RLGAMES_INPUT_PARSER
