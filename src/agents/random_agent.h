#ifndef RLGAMES_RANDOM_AGENT
#define RLGAMES_RANDOM_AGENT

#include <ctime>
#include <cstdlib>
#include <vector>
#include <random>
#include <algorithm>

#include <types.h>
#include <agent_base.h>

namespace rlgames {

namespace s = std;

//TODO: figure out if the template Board is really needed or not
template <typename Board, typename GameState>
struct RandomAgent :
    public AgentBase<Board, GameState, RandomAgent<Board, GameState>>
{
  RandomAgent() {
    s::srand(unsigned(s::time(0)));
  }

  [[nodiscard]] Move select_move(const GameState& gs) {
    if (mCache.size() == 0) {
      size_t sz = gs.board().size();
      initialize_cache(sz * sz);
    }
    IsPointAnEye<Board> is_point_an_eye;
    s::random_shuffle(
      s::begin(mCache), s::end(mCache),
      [](uint i){ return s::rand() % i; });  //TODO: avoid the %
    for (udyte idx : mCache) {
      Pt pt = point<Board::SIZE>(idx);
      Move m(M::Play, pt);
      if (gs.is_valid_move(m)
          and (not is_point_an_eye(gs.board(), pt, gs.next_player())))
        return m;
    }
    return Move(M::Pass);
  }

protected:
  void initialize_cache(size_t sz) {
    mCache.resize(sz);
    s::iota(mCache.begin(), mCache.end(), 0);
  }

private:
  s::vector<udyte> mCache;
};

}  // namespace rlgames

#endif  // RLGAMES_RANDOM_AGENT
