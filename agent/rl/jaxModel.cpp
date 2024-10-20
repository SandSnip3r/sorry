#include "jaxModel.hpp"

#include <iostream>

namespace py = pybind11;

namespace {

sorry::engine::Card cardIndexToCard(size_t index) {
  // Since the values of card enums are not a contiguous range, we can't just cast the index to a card.
  switch (index) {
    case 0: return sorry::engine::Card::kOne;
    case 1: return sorry::engine::Card::kTwo;
    case 2: return sorry::engine::Card::kThree;
    case 3: return sorry::engine::Card::kFour;
    case 4: return sorry::engine::Card::kFive;
    case 5: return sorry::engine::Card::kSeven;
    case 6: return sorry::engine::Card::kEight;
    case 7: return sorry::engine::Card::kTen;
    case 8: return sorry::engine::Card::kEleven;
    case 9: return sorry::engine::Card::kTwelve;
    case 10: return sorry::engine::Card::kSorry;
    default: throw std::runtime_error("Invalid card index");
  }
}

}

JaxModel::JaxModel(py::module &jaxModule) {
  // Instantiate the MyModel class from Python
  py::object TestClass = jaxModule.attr("TestClass");
  // Create an instance of MyModel
  modelInstance_ = TestClass();
}

void JaxModel::setSeed(int seed) {
  // TODO
}

sorry::engine::Action JaxModel::getAction(const std::array<sorry::engine::Card, 5> &playerHand, const std::array<int, 4> &playerPiecePositions) {
  const py::array_t<float> numpyObservation = observationToNumpyArray(playerHand, playerPiecePositions);
  // Take an action according to the policy
  py::object res = modelInstance_.attr("getAction")(numpyObservation);
  return numpyArrayToAction(py::cast<py::array_t<float>>(res));
}

void JaxModel::train(const JaxTrajectory &trajectory) {
  // TODO
}

py::array_t<float> JaxModel::observationToNumpyArray(const std::array<sorry::engine::Card, 5> &playerHand,
                                          const std::array<int, 4> &playerPiecePositions) const {
  auto printBuffer = [](const auto &buffer) {
    const auto info = buffer.request();
    std::cout << "Buffer info: " << std::endl;
    std::cout << "  ptr: " << info.ptr << std::endl;
    std::cout << "  itemsize: " << info.itemsize << std::endl;
    std::cout << "  size: " << info.size << std::endl;
    std::cout << "  ndim: " << info.ndim << std::endl;
    std::cout << "  shape: [ ";
    for (size_t i=0; i<info.shape.size(); ++i) {
      std::cout << info.shape[i] << " ";
    }
    std::cout << "]" << std::endl;
    std::cout << "  strides: [ ";
    for (size_t i=0; i<info.strides.size(); ++i) {
      std::cout << info.strides[i] << " ";
    }
    std::cout << "]" << std::endl;
  };

  constexpr std::size_t handCardCount = std::decay_t<decltype(playerHand)>().size();
  constexpr std::size_t cardTypeCount = 11;
  constexpr std::size_t pieceCount = std::decay_t<decltype(playerPiecePositions)>().size();
  constexpr std::size_t piecePositionTypeCount = 67; // TODO: This will need to change once we have more than one player.

  // Declare a numpy array for the entire observation.
  // The version below does not work on 2.9.1-2 but does on 2.11.1-2, it seems to result in a memory layout which does not allow for slicing views.
  py::array_t<float> numpyObservation(handCardCount * cardTypeCount + pieceCount * piecePositionTypeCount);
  // The version below worked when I was using pybind11 2.9.1-2.
  // py::array_t<float> numpyObservation({handCardCount * cardTypeCount + pieceCount * piecePositionTypeCount}, {sizeof(float)});

  // printBuffer(numpyObservation);

  // Initialize to all 0s, since we're filling a lot of one-hot vectors; most elements will be 0.
  numpyObservation.attr("fill")(0.0);

  // The first part of the observation contains the player's hand. We'll use a one-hot encoding of the card type for the 5 cards.
  py::slice handSliceDims(0, handCardCount*cardTypeCount, 1);
  py::array handSlice = numpyObservation[handSliceDims];
  py::array_t<float> reshapedHandSlice = py::cast<py::array_t<float>>(handSlice.attr("reshape")(handCardCount, cardTypeCount));
  auto uncheckedHandBuffer = reshapedHandSlice.mutable_unchecked<2>();
  for (size_t cardIndex=0; cardIndex<playerHand.size(); ++cardIndex) {
    uncheckedHandBuffer(cardIndex, static_cast<int>(playerHand[cardIndex])) = 1.0;
  }

  // The second part of the observation contains the player's piece positions. We'll use a one-hot encoding of the position type for the 4 pieces.
  py::slice pieceSliceDims(handCardCount*cardTypeCount, handCardCount*cardTypeCount + pieceCount*piecePositionTypeCount, 1);
  py::array pieceSlice = numpyObservation[pieceSliceDims];
  py::array_t<float> reshapedPieceSlice = py::cast<py::array_t<float>>(pieceSlice.attr("reshape")(pieceCount, piecePositionTypeCount));
  auto uncheckedPiecesBuffer = reshapedPieceSlice.mutable_unchecked<2>();
  for (size_t pieceIndex=0; pieceIndex<playerPiecePositions.size(); ++pieceIndex) {
    uncheckedPiecesBuffer(pieceIndex, playerPiecePositions[pieceIndex]) = 1.0;
  }

  return numpyObservation;
}

sorry::engine::Action JaxModel::numpyArrayToAction(const py::array_t<float> &numpyActionVector) const {
  std::cout << "Received a vector of size " << numpyActionVector.size() << std::endl;
  const sorry::engine::PlayerColor playerColor = sorry::engine::PlayerColor::kYellow;
  // For now, we're expecting a flat vector of all possible actions.
  // TODO: Maybe we use argmax here, or maybe the model returns the argmax, not sure yet.
  const py::object indexPyObject = py::module::import("numpy").attr("nanargmax")(numpyActionVector);
  const size_t index = indexPyObject.cast<size_t>();

  using sorry::engine::Action;

  struct ActionRange {
    ActionRange(size_t start, size_t end, std::function<Action(size_t)> actionGenerator)
      : start(start), end(end), actionGenerator(actionGenerator) {}
    size_t start;
    size_t end;
    std::function<Action(size_t)> actionGenerator;
  };

  // Define actions for all values of `index`.
  std::vector<ActionRange> actionRanges;
  auto addActions = [&actionRanges](size_t count, std::function<Action(size_t)> actionGenerator) {
    size_t startOfRange = actionRanges.empty() ? 0 : actionRanges.back().end;
    actionRanges.emplace_back(startOfRange, startOfRange+count, actionGenerator);
  };

  // ========================================= DISCARD =========================================
  // The first 11 actions are for discarding specific card types.
  addActions(11, [&](size_t index) { return Action::discard(playerColor, cardIndexToCard(index)); });

  // ===================================== MOVE FROM START =====================================
  // The next action is for using a 1 card to move a piece out of Start.
  addActions(1, [&](size_t index) {
    (void)index;
    int pieceIndex = 0; // TODO: Get piece index
    int firstPositionFromStart = 1; // TODO: Get destination position
    return Action::singleMove(playerColor, sorry::engine::Card::kOne, pieceIndex, firstPositionFromStart);
  });

  // The next action is for using a 2 card to move a piece out of Start.
  addActions(1, [&](size_t index) {
    (void)index;
    int pieceIndex = 0; // TODO: Get piece index
    int firstPositionFromStart = 1; // TODO: Get destination position
    return Action::singleMove(playerColor, sorry::engine::Card::kTwo, pieceIndex, firstPositionFromStart);
  });

  // ========================================== SORRY ==========================================
  // The next 60 actions are for using a Sorry card on a specific target public position.
  addActions(60, [&](size_t index) {
    return Action::sorry(playerColor, index);
  });

  // ========================================== SWAP ===========================================
  // The next 3540(60*59) actions are for using an 11 card to swap a piece on a specific public position with an opponent piece on a different specific public position.
  addActions(60*59, [&](size_t index) {
    // index == sourcePosition * 59 + targetPosition
    int sourcePosition = index / 59;
    int targetPosition = index % 59;
    int pieceIndex = 0; // TODO: Get piece index from `sourcePosition`.
    return Action::swap(playerColor, pieceIndex, targetPosition);
  });

  // ====================================== SINGLE FOWARD ======================================
  // The next 65 actions are for using a 1 card to move forward 1, starting from a specific position in the range [1,65].
  addActions(65, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 1;
    return Action::singleMove(playerColor, sorry::engine::Card::kOne, pieceIndex, destination);
  });

  // The next 64 actions are for using a 2 card to move forward 2, starting from a specific position in the range [1,64].
  addActions(64, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 2;
    return Action::singleMove(playerColor, sorry::engine::Card::kTwo, pieceIndex, destination);
  });

  // The next 63 actions are for using a 3 card to move forward 3, starting from a specific position in the range [1,63].
  addActions(63, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 3;
    return Action::singleMove(playerColor, sorry::engine::Card::kThree, pieceIndex, destination);
  });

  // The next 62 actions are for using a Sorry card to move forward 4, starting from a specific position in the range [1,62].
  addActions(62, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 4;
    return Action::singleMove(playerColor, sorry::engine::Card::kSorry, pieceIndex, destination);
  });

  // The next 61 actions are for using a 5 card to move forward 5, starting from a specific position in the range [1,61].
  addActions(61, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 5;
    return Action::singleMove(playerColor, sorry::engine::Card::kFive, pieceIndex, destination);
  });

  // The next 59 actions are for using a 7 card to move forward 7, starting from a specific position in the range [1,59].
  addActions(59, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 7;
    return Action::singleMove(playerColor, sorry::engine::Card::kSeven, pieceIndex, destination);
  });

  // The next 58 actions are for using a 8 card to move forward 8, starting from a specific position in the range [1,58].
  addActions(58, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 8;
    return Action::singleMove(playerColor, sorry::engine::Card::kEight, pieceIndex, destination);
  });

  // The next 56 actions are for using a 10 card to move forward 10, starting from a specific position in the range [1,56].
  addActions(56, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 10;
    return Action::singleMove(playerColor, sorry::engine::Card::kTen, pieceIndex, destination);
  });

  // The next 55 actions are for using a 11 card to move forward 11, starting from a specific position in the range [1,55].
  addActions(55, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 11;
    return Action::singleMove(playerColor, sorry::engine::Card::kEleven, pieceIndex, destination);
  });

  // The next 54 actions are for using a 12 card to move forward 12, starting from a specific position in the range [1,54].
  addActions(54, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition + 12;
    return Action::singleMove(playerColor, sorry::engine::Card::kTwelve, pieceIndex, destination);
  });

  // ===================================== SINGLE BACKWARD =====================================
  // The next 65 actions are for using a 10 card to move backward 1, starting from a specific position in the range [1,65].
  addActions(65, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition - 1; // TODO: Compute wrap-around
    return Action::singleMove(playerColor, sorry::engine::Card::kTen, pieceIndex, destination);
  });

  // The next 65 actions are for using a 4 card to move backward 4, starting from a specific position in the range [1,65].
  addActions(65, [&](size_t index) {
    int startingPosition = 1 + index;
    int pieceIndex = 0; // TODO: Get piece index
    int destination = startingPosition - 4; // TODO: Compute wrap-around
    return Action::singleMove(playerColor, sorry::engine::Card::kFour, pieceIndex, destination);
  });

  // =================================== SEVEN - DOUBLE MOVE ===================================
  // First 1, second 6
  // The next 3900(65*60) actions are for using a 7 card to move one piece forward 1, starting from a specific position in the range [1,65] and a second piece forward 6, starting from a specific position in the range [1,60].
  // Note: This range includes the possibility of specifying the same source twice (60 invalid actions) as well as the same destination twice (60-1 invalid actions; both ending on 66 is valid).
  addActions(65*60, [&](size_t index) {
    // index == piece1Position * 60 + piece2Position
    int piece1Position = index / 60;
    int piece1Index = 0; // TODO: Get piece index
    int piece2Position = index % 60;
    int piece2Index = 0; // TODO: Get piece index
    return Action::doubleMove(playerColor, sorry::engine::Card::kSeven, piece1Index, piece1Position, piece2Index, piece2Position);
  });

  // First 2, second 5
  // The next 3904(64*61) actions are for using a 7 card to move one piece forward 2, starting from a specific position in the range [1,64] and a second piece forward 5, starting from a specific position in the range [1,61].
  // Note: This range includes the possibility of specifying the same source twice (61 invalid actions) as well as the same destination twice (61-1 invalid actions; both ending on 66 is valid).
  addActions(64*61, [&](size_t index) {
    // index == piece1Position * 61 + piece2Position
    int piece1Position = index / 61;
    int piece1Index = 0; // TODO: Get piece index
    int piece2Position = index % 61;
    int piece2Index = 0; // TODO: Get piece index
    return Action::doubleMove(playerColor, sorry::engine::Card::kSeven, piece1Index, piece1Position, piece2Index, piece2Position);
  });

  // First 3, second 4
  // The next 3904(63*62) actions are for using a 7 card to move one piece forward 3, starting from a specific position in the range [1,63] and a second piece forward 4, starting from a specific position in the range [1,62].
  // Note: This range includes the possibility of specifying the same source twice (62 invalid actions) as well as the same destination twice (62-1 invalid actions; both ending on 66 is valid).
  addActions(63*62, [&](size_t index) {
    // index == piece1Position * 62 + piece2Position
    int piece1Position = index / 62;
    int piece1Index = 0; // TODO: Get piece index
    int piece2Position = index % 62;
    int piece2Index = 0; // TODO: Get piece index
    return Action::doubleMove(playerColor, sorry::engine::Card::kSeven, piece1Index, piece1Position, piece2Index, piece2Position);
  });

  // ===========================================================================================

  std::cout << "Final action count: " << actionRanges.back().end << std::endl;

  for (const ActionRange &range : actionRanges) {
    if (index >= range.start && index < range.end) {
      return range.actionGenerator(index - range.start);
    }
  }
  throw std::runtime_error("Action index " + std::to_string(index) + " is out of range");
}