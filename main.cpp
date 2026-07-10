#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace crepusculo {

constexpr int Empty = 0;
constexpr int Pawn = 1;
constexpr int Knight = 2;
constexpr int Bishop = 3;
constexpr int Rook = 4;
constexpr int Queen = 5;
constexpr int King = 6;

constexpr int White = 1;
constexpr int Black = -1;

constexpr int CastleWK = 1;
constexpr int CastleWQ = 2;
constexpr int CastleBK = 4;
constexpr int CastleBQ = 8;

constexpr int MateScore = 30000;
constexpr int Inf = 32000;

const std::string StartFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

int file_of(int sq) { return sq & 7; }
int rank_of(int sq) { return sq >> 3; }
bool on_board(int file, int rank) { return file >= 0 && file < 8 && rank >= 0 && rank < 8; }
int make_sq(int file, int rank) { return rank * 8 + file; }

char piece_to_char(int piece) {
    switch (piece) {
    case Pawn: return 'P';
    case Knight: return 'N';
    case Bishop: return 'B';
    case Rook: return 'R';
    case Queen: return 'Q';
    case King: return 'K';
    case -Pawn: return 'p';
    case -Knight: return 'n';
    case -Bishop: return 'b';
    case -Rook: return 'r';
    case -Queen: return 'q';
    case -King: return 'k';
    default: return '.';
    }
}

int char_to_piece(char c) {
    switch (c) {
    case 'P': return Pawn;
    case 'N': return Knight;
    case 'B': return Bishop;
    case 'R': return Rook;
    case 'Q': return Queen;
    case 'K': return King;
    case 'p': return -Pawn;
    case 'n': return -Knight;
    case 'b': return -Bishop;
    case 'r': return -Rook;
    case 'q': return -Queen;
    case 'k': return -King;
    default: return Empty;
    }
}

std::string square_to_string(int sq) {
    std::string s = "a1";
    s[0] = static_cast<char>('a' + file_of(sq));
    s[1] = static_cast<char>('1' + rank_of(sq));
    return s;
}

std::optional<int> parse_square(const std::string& s) {
    if (s.size() != 2 || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') {
        return std::nullopt;
    }
    return make_sq(s[0] - 'a', s[1] - '1');
}

struct Move {
    int from = 0;
    int to = 0;
    int promotion = Empty;
    bool enPassant = false;
    bool castle = false;

    bool operator==(const Move& other) const {
        return from == other.from && to == other.to && promotion == other.promotion &&
               enPassant == other.enPassant && castle == other.castle;
    }
};

struct Position;

std::uint64_t recompute_hash(const Position& pos);
std::uint64_t hash_key(const Position& pos);
std::uint64_t zobrist_piece_key(int piece, int sq);
std::uint64_t zobrist_castling_key(int castling);
std::uint64_t zobrist_ep_key(int file);
std::uint64_t zobrist_side_key();
bool has_en_passant_capturer(const Position& pos);

std::string move_to_uci(const Move& move) {
    std::string out = square_to_string(move.from) + square_to_string(move.to);
    if (move.promotion != Empty) {
        char p = static_cast<char>(std::tolower(piece_to_char(move.promotion)));
        out.push_back(p);
    }
    return out;
}

std::uint16_t pack_move(const Move& move) {
    return static_cast<std::uint16_t>(move.from | (move.to << 6) | (move.promotion << 12));
}

bool matches_packed_move(const Move& move, std::uint16_t packed) {
    return packed != 0 && pack_move(move) == packed;
}

struct Undo {
    int captured = Empty;
    int moved = Empty;
    int castling = 0;
    int ep = -1;
    int halfmove = 0;
    int fullmove = 1;
    std::uint64_t key = 0;
    std::array<int, 2> kingSq{};
};

struct Position {
    std::array<int, 64> board{};
    std::array<int, 2> kingSq{make_sq(4, 0), make_sq(4, 7)};
    int side = White;
    int castling = CastleWK | CastleWQ | CastleBK | CastleBQ;
    int ep = -1;
    int halfmove = 0;
    int fullmove = 1;
    std::uint64_t key = 0;

    void clear() {
        board.fill(Empty);
        kingSq = {-1, -1};
        side = White;
        castling = 0;
        ep = -1;
        halfmove = 0;
        fullmove = 1;
        key = 0;
    }

    bool set_fen(const std::string& fen) {
        clear();
        std::istringstream in(fen);
        std::string placement, stm, castles, epToken;
        if (!(in >> placement >> stm >> castles >> epToken >> halfmove >> fullmove)) {
            return false;
        }

        int rank = 7;
        int file = 0;
        for (char c : placement) {
            if (c == '/') {
                if (file != 8) return false;
                --rank;
                file = 0;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                file += c - '0';
                if (file > 8) return false;
                continue;
            }
            int piece = char_to_piece(c);
            if (piece == Empty || rank < 0 || file >= 8) return false;
            board[make_sq(file, rank)] = piece;
            ++file;
        }
        if (rank != 0 || file != 8) return false;

        if (stm == "w") side = White;
        else if (stm == "b") side = Black;
        else return false;

        castling = 0;
        if (castles != "-") {
            for (char c : castles) {
                if (c == 'K') castling |= CastleWK;
                else if (c == 'Q') castling |= CastleWQ;
                else if (c == 'k') castling |= CastleBK;
                else if (c == 'q') castling |= CastleBQ;
                else return false;
            }
        }

        ep = -1;
        if (epToken != "-") {
            auto sq = parse_square(epToken);
            if (!sq) return false;
            ep = *sq;
        }

        int whiteKings = 0;
        int blackKings = 0;
        for (int sq = 0; sq < 64; ++sq) {
            int piece = board[sq];
            if (piece == King) {
                ++whiteKings;
                kingSq[0] = sq;
            } else if (piece == -King) {
                ++blackKings;
                kingSq[1] = sq;
            }
        }
        if (whiteKings != 1 || blackKings != 1) return false;

        sanitize_castling_rights();
        key = recompute_hash(*this);
        return true;
    }

    void set_startpos() { set_fen(StartFen); }

    int king_square(int color) const {
        return kingSq[color == White ? 0 : 1];
    }

    bool square_attacked(int sq, int bySide) const {
        int f = file_of(sq);
        int r = rank_of(sq);

        if (bySide == White) {
            if (f < 7 && r > 0 && board[sq - 7] == Pawn) return true;
            if (f > 0 && r > 0 && board[sq - 9] == Pawn) return true;
        } else {
            if (f > 0 && r < 7 && board[sq + 7] == -Pawn) return true;
            if (f < 7 && r < 7 && board[sq + 9] == -Pawn) return true;
        }

        constexpr int knightDeltas[8][2] = {
            {1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}
        };
        for (auto& d : knightDeltas) {
            int nf = f + d[0];
            int nr = r + d[1];
            if (on_board(nf, nr) && board[make_sq(nf, nr)] == bySide * Knight) return true;
        }

        constexpr int bishopDeltas[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        for (auto& d : bishopDeltas) {
            int nf = f + d[0];
            int nr = r + d[1];
            while (on_board(nf, nr)) {
                int p = board[make_sq(nf, nr)];
                if (p != Empty) {
                    if (p == bySide * Bishop || p == bySide * Queen) return true;
                    break;
                }
                nf += d[0];
                nr += d[1];
            }
        }

        constexpr int rookDeltas[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto& d : rookDeltas) {
            int nf = f + d[0];
            int nr = r + d[1];
            while (on_board(nf, nr)) {
                int p = board[make_sq(nf, nr)];
                if (p != Empty) {
                    if (p == bySide * Rook || p == bySide * Queen) return true;
                    break;
                }
                nf += d[0];
                nr += d[1];
            }
        }

        for (int df = -1; df <= 1; ++df) {
            for (int dr = -1; dr <= 1; ++dr) {
                if (df == 0 && dr == 0) continue;
                int nf = f + df;
                int nr = r + dr;
                if (on_board(nf, nr) && board[make_sq(nf, nr)] == bySide * King) return true;
            }
        }
        return false;
    }

    bool in_check(int color) const {
        int kingSq = king_square(color);
        return kingSq >= 0 && square_attacked(kingSq, -color);
    }

    void update_castling_rights(int from, int to, int moved, int captured) {
        if (moved == King) castling &= ~(CastleWK | CastleWQ);
        if (moved == -King) castling &= ~(CastleBK | CastleBQ);
        if (from == make_sq(0, 0) || (captured == Rook && to == make_sq(0, 0))) castling &= ~CastleWQ;
        if (from == make_sq(7, 0) || (captured == Rook && to == make_sq(7, 0))) castling &= ~CastleWK;
        if (from == make_sq(0, 7) || (captured == -Rook && to == make_sq(0, 7))) castling &= ~CastleBQ;
        if (from == make_sq(7, 7) || (captured == -Rook && to == make_sq(7, 7))) castling &= ~CastleBK;
    }

    void sanitize_castling_rights() {
        if (board[make_sq(4, 0)] != King) castling &= ~(CastleWK | CastleWQ);
        if (board[make_sq(4, 7)] != -King) castling &= ~(CastleBK | CastleBQ);
        if (board[make_sq(7, 0)] != Rook) castling &= ~CastleWK;
        if (board[make_sq(0, 0)] != Rook) castling &= ~CastleWQ;
        if (board[make_sq(7, 7)] != -Rook) castling &= ~CastleBK;
        if (board[make_sq(0, 7)] != -Rook) castling &= ~CastleBQ;
    }

    Undo make_move(const Move& move) {
        Undo undo{board[move.to], board[move.from], castling, ep, halfmove, fullmove, key, kingSq};
        int moved = board[move.from];
        int mover = moved > 0 ? White : Black;
        int captured = board[move.to];
        if (has_en_passant_capturer(*this)) key ^= zobrist_ep_key(file_of(ep));
        key ^= zobrist_castling_key(castling);
        key ^= zobrist_piece_key(moved, move.from);

        board[move.from] = Empty;
        if (move.enPassant) {
            int capSq = move.to - mover * 8;
            captured = board[capSq];
            undo.captured = captured;
            key ^= zobrist_piece_key(captured, capSq);
            board[capSq] = Empty;
        } else if (captured != Empty) {
            key ^= zobrist_piece_key(captured, move.to);
        }

        int placed = moved;
        if (move.promotion != Empty) {
            placed = mover * move.promotion;
        }
        board[move.to] = placed;
        if (std::abs(moved) == King) kingSq[mover == White ? 0 : 1] = move.to;
        key ^= zobrist_piece_key(placed, move.to);

        if (move.castle) {
            if (move.to == make_sq(6, 0)) {
                key ^= zobrist_piece_key(Rook, make_sq(7, 0));
                key ^= zobrist_piece_key(Rook, make_sq(5, 0));
                board[make_sq(5, 0)] = board[make_sq(7, 0)];
                board[make_sq(7, 0)] = Empty;
            } else if (move.to == make_sq(2, 0)) {
                key ^= zobrist_piece_key(Rook, make_sq(0, 0));
                key ^= zobrist_piece_key(Rook, make_sq(3, 0));
                board[make_sq(3, 0)] = board[make_sq(0, 0)];
                board[make_sq(0, 0)] = Empty;
            } else if (move.to == make_sq(6, 7)) {
                key ^= zobrist_piece_key(-Rook, make_sq(7, 7));
                key ^= zobrist_piece_key(-Rook, make_sq(5, 7));
                board[make_sq(5, 7)] = board[make_sq(7, 7)];
                board[make_sq(7, 7)] = Empty;
            } else if (move.to == make_sq(2, 7)) {
                key ^= zobrist_piece_key(-Rook, make_sq(0, 7));
                key ^= zobrist_piece_key(-Rook, make_sq(3, 7));
                board[make_sq(3, 7)] = board[make_sq(0, 7)];
                board[make_sq(0, 7)] = Empty;
            }
        }

        update_castling_rights(move.from, move.to, moved, captured);
        key ^= zobrist_castling_key(castling);
        ep = -1;
        if (std::abs(moved) == Pawn && std::abs(move.to - move.from) == 16) {
            ep = (move.from + move.to) / 2;
        }

        if (std::abs(moved) == Pawn || captured != Empty) halfmove = 0;
        else ++halfmove;
        if (side == Black) ++fullmove;
        side = -side;
        key ^= zobrist_side_key();
        if (has_en_passant_capturer(*this)) key ^= zobrist_ep_key(file_of(ep));
        return undo;
    }

    void unmake_move(const Move& move, const Undo& undo) {
        side = -side;
        int mover = side;
        board[move.from] = undo.moved;
        board[move.to] = undo.captured;

        if (move.enPassant) {
            int capSq = move.to - mover * 8;
            board[move.to] = Empty;
            board[capSq] = undo.captured;
        }

        if (move.castle) {
            if (move.to == make_sq(6, 0)) {
                board[make_sq(7, 0)] = board[make_sq(5, 0)];
                board[make_sq(5, 0)] = Empty;
            } else if (move.to == make_sq(2, 0)) {
                board[make_sq(0, 0)] = board[make_sq(3, 0)];
                board[make_sq(3, 0)] = Empty;
            } else if (move.to == make_sq(6, 7)) {
                board[make_sq(7, 7)] = board[make_sq(5, 7)];
                board[make_sq(5, 7)] = Empty;
            } else if (move.to == make_sq(2, 7)) {
                board[make_sq(0, 7)] = board[make_sq(3, 7)];
                board[make_sq(3, 7)] = Empty;
            }
        }

        castling = undo.castling;
        ep = undo.ep;
        halfmove = undo.halfmove;
        fullmove = undo.fullmove;
        key = undo.key;
        kingSq = undo.kingSq;
    }
};

std::uint64_t splitmix64(std::uint64_t& x) {
    x += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

int piece_index(int piece) {
    int offset = piece > 0 ? 0 : 6;
    return offset + std::abs(piece) - 1;
}

struct Zobrist {
    std::array<std::array<std::uint64_t, 64>, 12> pieces{};
    std::array<std::uint64_t, 16> castling{};
    std::array<std::uint64_t, 8> epFile{};
    std::uint64_t side = 0;

    Zobrist() {
        std::uint64_t seed = 0x4155524f52415f31ULL;
        for (auto& pieceKeys : pieces) {
            for (auto& key : pieceKeys) key = splitmix64(seed);
        }
        for (auto& key : castling) key = splitmix64(seed);
        for (auto& key : epFile) key = splitmix64(seed);
        side = splitmix64(seed);
    }
};

const Zobrist& zobrist() {
    static const Zobrist keys;
    return keys;
}

std::uint64_t zobrist_piece_key(int piece, int sq) {
    return zobrist().pieces[piece_index(piece)][sq];
}

std::uint64_t zobrist_castling_key(int castling) {
    return zobrist().castling[castling & 15];
}

std::uint64_t zobrist_ep_key(int file) {
    return zobrist().epFile[file];
}

std::uint64_t zobrist_side_key() {
    return zobrist().side;
}

bool has_en_passant_capturer(const Position& pos) {
    if (pos.ep < 0) return false;

    int epFile = file_of(pos.ep);
    int epRank = rank_of(pos.ep);
    int pawnRank = epRank - pos.side;
    int capturedSq = pos.ep - pos.side * 8;
    if (!on_board(epFile, pawnRank) || capturedSq < 0 || capturedSq >= 64) return false;
    if (pos.board[capturedSq] != -pos.side * Pawn) return false;

    for (int df : {-1, 1}) {
        int pawnFile = epFile + df;
        if (!on_board(pawnFile, pawnRank)) continue;
        if (pos.board[make_sq(pawnFile, pawnRank)] == pos.side * Pawn) return true;
    }
    return false;
}

std::uint64_t recompute_hash(const Position& pos) {
    const Zobrist& z = zobrist();
    std::uint64_t key = 0;
    for (int sq = 0; sq < 64; ++sq) {
        int piece = pos.board[sq];
        if (piece != Empty) key ^= z.pieces[piece_index(piece)][sq];
    }
    if (pos.side == Black) key ^= z.side;
    key ^= z.castling[pos.castling & 15];
    if (has_en_passant_capturer(pos)) key ^= z.epFile[file_of(pos.ep)];
    return key;
}

std::uint64_t hash_key(const Position& pos) {
    return pos.key;
}

bool insufficient_material(const Position& pos) {
    int minors = 0;
    int knights = 0;
    std::array<int, 2> bishopsByColor{};

    for (int sq = 0; sq < 64; ++sq) {
        int piece = pos.board[sq];
        if (piece == Empty || std::abs(piece) == King) continue;

        int type = std::abs(piece);
        if (type == Pawn || type == Rook || type == Queen) return false;
        if (type == Knight) {
            ++knights;
            ++minors;
        } else if (type == Bishop) {
            ++bishopsByColor[(file_of(sq) + rank_of(sq)) & 1];
            ++minors;
        }
    }

    if (minors == 0) return true;
    if (minors == 1) return true;

    if (knights == 0) {
        return bishopsByColor[0] == 0 || bishopsByColor[1] == 0;
    }

    return false;
}

struct MoveList {
    std::array<Move, 256> moves;
    size_t count;

    MoveList() : count(0) {}

    void push_back(const Move& move) {
        if (count < moves.size()) moves[count++] = move;
    }

    bool empty() const { return count == 0; }
    size_t size() const { return count; }
    Move& front() { return moves[0]; }
    const Move& front() const { return moves[0]; }
    Move& operator[](size_t index) { return moves[index]; }
    const Move& operator[](size_t index) const { return moves[index]; }
    Move* begin() { return moves.data(); }
    Move* end() { return moves.data() + count; }
    const Move* begin() const { return moves.data(); }
    const Move* end() const { return moves.data() + count; }
};

class MoveGen {
public:
    static MoveList legal_moves(Position& pos) {
        MoveList pseudo;
        generate_pseudo(pos, pseudo, false);
        MoveList legal;
        int mover = pos.side;
        for (const Move& move : pseudo) {
            Undo undo = pos.make_move(move);
            if (!pos.in_check(mover)) legal.push_back(move);
            pos.unmake_move(move, undo);
        }
        return legal;
    }

    static MoveList legal_captures(Position& pos) {
        MoveList pseudo;
        generate_pseudo(pos, pseudo, true);
        MoveList legal;
        int mover = pos.side;
        for (const Move& move : pseudo) {
            Undo undo = pos.make_move(move);
            if (!pos.in_check(mover)) legal.push_back(move);
            pos.unmake_move(move, undo);
        }
        return legal;
    }

private:
    static void add_promotion_moves(MoveList& moves, int from, int to, bool ep = false) {
        for (int promo : {Queen, Rook, Bishop, Knight}) {
            moves.push_back(Move{from, to, promo, ep, false});
        }
    }

    static void add_move(MoveList& moves, int from, int to, int promotion = Empty, bool ep = false, bool castle = false) {
        moves.push_back(Move{from, to, promotion, ep, castle});
    }

    static void generate_pseudo(Position& pos, MoveList& moves, bool capturesOnly) {
        int us = pos.side;
        for (int from = 0; from < 64; ++from) {
            int piece = pos.board[from];
            if (piece == Empty || (piece > 0 ? White : Black) != us) continue;
            int type = std::abs(piece);
            int f = file_of(from);
            int r = rank_of(from);

            if (type == Pawn) {
                int dir = us == White ? 1 : -1;
                int oneRank = r + dir;
                int promotionRank = us == White ? 7 : 0;
                int startRank = us == White ? 1 : 6;
                if (on_board(f, oneRank)) {
                    int to = make_sq(f, oneRank);
                    if (pos.board[to] == Empty) {
                        if (oneRank == promotionRank) {
                            add_promotion_moves(moves, from, to);
                        } else if (!capturesOnly) {
                            add_move(moves, from, to);
                        }

                        int twoRank = r + 2 * dir;
                        if (!capturesOnly && r == startRank && on_board(f, twoRank)) {
                            int two = make_sq(f, twoRank);
                            if (pos.board[two] == Empty) add_move(moves, from, two);
                        }
                    }
                }

                for (int df : {-1, 1}) {
                    int nf = f + df;
                    int nr = r + dir;
                    if (!on_board(nf, nr)) continue;
                    int to = make_sq(nf, nr);
                    int target = pos.board[to];
                    if (target != Empty && (target > 0 ? White : Black) == -us) {
                        if (nr == promotionRank) add_promotion_moves(moves, from, to);
                        else add_move(moves, from, to);
                    } else if (to == pos.ep) {
                        add_move(moves, from, to, Empty, true);
                    }
                }
            } else if (type == Knight) {
                constexpr int deltas[8][2] = {
                    {1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}
                };
                for (auto& d : deltas) {
                    int nf = f + d[0];
                    int nr = r + d[1];
                    if (!on_board(nf, nr)) continue;
                    int to = make_sq(nf, nr);
                    int target = pos.board[to];
                    if (target == Empty) {
                        if (!capturesOnly) add_move(moves, from, to);
                    } else if ((target > 0 ? White : Black) == -us) {
                        add_move(moves, from, to);
                    }
                }
            } else if (type == Bishop || type == Rook || type == Queen) {
                static constexpr int dirs[8][2] = {
                    {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
                };
                int begin = type == Bishop ? 4 : 0;
                int end = type == Rook ? 4 : 8;
                for (int i = begin; i < end; ++i) {
                    int nf = f + dirs[i][0];
                    int nr = r + dirs[i][1];
                    while (on_board(nf, nr)) {
                        int to = make_sq(nf, nr);
                        int target = pos.board[to];
                        if (target == Empty) {
                            if (!capturesOnly) add_move(moves, from, to);
                        } else {
                            if ((target > 0 ? White : Black) == -us) add_move(moves, from, to);
                            break;
                        }
                        nf += dirs[i][0];
                        nr += dirs[i][1];
                    }
                }
            } else if (type == King) {
                for (int df = -1; df <= 1; ++df) {
                    for (int dr = -1; dr <= 1; ++dr) {
                        if (df == 0 && dr == 0) continue;
                        int nf = f + df;
                        int nr = r + dr;
                        if (!on_board(nf, nr)) continue;
                        int to = make_sq(nf, nr);
                        int target = pos.board[to];
                        if (target == Empty) {
                            if (!capturesOnly) add_move(moves, from, to);
                        } else if ((target > 0 ? White : Black) == -us) {
                            add_move(moves, from, to);
                        }
                    }
                }

                if (!capturesOnly && !pos.in_check(us)) {
                    if (us == White && from == make_sq(4, 0)) {
                        if ((pos.castling & CastleWK) && pos.board[make_sq(5, 0)] == Empty && pos.board[make_sq(6, 0)] == Empty &&
                            !pos.square_attacked(make_sq(5, 0), Black) && !pos.square_attacked(make_sq(6, 0), Black)) {
                            add_move(moves, from, make_sq(6, 0), Empty, false, true);
                        }
                        if ((pos.castling & CastleWQ) && pos.board[make_sq(3, 0)] == Empty && pos.board[make_sq(2, 0)] == Empty && pos.board[make_sq(1, 0)] == Empty &&
                            !pos.square_attacked(make_sq(3, 0), Black) && !pos.square_attacked(make_sq(2, 0), Black)) {
                            add_move(moves, from, make_sq(2, 0), Empty, false, true);
                        }
                    } else if (us == Black && from == make_sq(4, 7)) {
                        if ((pos.castling & CastleBK) && pos.board[make_sq(5, 7)] == Empty && pos.board[make_sq(6, 7)] == Empty &&
                            !pos.square_attacked(make_sq(5, 7), White) && !pos.square_attacked(make_sq(6, 7), White)) {
                            add_move(moves, from, make_sq(6, 7), Empty, false, true);
                        }
                        if ((pos.castling & CastleBQ) && pos.board[make_sq(3, 7)] == Empty && pos.board[make_sq(2, 7)] == Empty && pos.board[make_sq(1, 7)] == Empty &&
                            !pos.square_attacked(make_sq(3, 7), White) && !pos.square_attacked(make_sq(2, 7), White)) {
                            add_move(moves, from, make_sq(2, 7), Empty, false, true);
                        }
                    }
                }
            }
        }
    }
};

int piece_value(int piece) {
    switch (std::abs(piece)) {
    case Pawn: return 100;
    case Knight: return 320;
    case Bishop: return 330;
    case Rook: return 500;
    case Queen: return 900;
    default: return 0;
    }
}

int pseudo_mobility(const Position& pos, int side) {
    int mobility = 0;
    for (int sq = 0; sq < 64; ++sq) {
        int piece = pos.board[sq];
        if (piece == Empty || (piece > 0 ? White : Black) != side) continue;
        int type = std::abs(piece);
        int f = file_of(sq);
        int r = rank_of(sq);

        if (type == Pawn) {
            int dir = side == White ? 1 : -1;
            for (int df : {-1, 1}) {
                int nf = f + df;
                int nr = r + dir;
                if (on_board(nf, nr)) ++mobility;
            }
        } else if (type == Knight) {
            constexpr int deltas[8][2] = {
                {1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}
            };
            for (auto& d : deltas) {
                int nf = f + d[0];
                int nr = r + d[1];
                if (!on_board(nf, nr)) continue;
                int target = pos.board[make_sq(nf, nr)];
                if (target == Empty || (target > 0 ? White : Black) != side) ++mobility;
            }
        } else if (type == Bishop || type == Rook || type == Queen) {
            static constexpr int dirs[8][2] = {
                {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
            };
            int begin = type == Bishop ? 4 : 0;
            int end = type == Rook ? 4 : 8;
            for (int i = begin; i < end; ++i) {
                int nf = f + dirs[i][0];
                int nr = r + dirs[i][1];
                while (on_board(nf, nr)) {
                    int target = pos.board[make_sq(nf, nr)];
                    if (target == Empty) {
                        ++mobility;
                    } else {
                        if ((target > 0 ? White : Black) != side) ++mobility;
                        break;
                    }
                    nf += dirs[i][0];
                    nr += dirs[i][1];
                }
            }
        } else if (type == King) {
            for (int df = -1; df <= 1; ++df) {
                for (int dr = -1; dr <= 1; ++dr) {
                    if (df == 0 && dr == 0) continue;
                    int nf = f + df;
                    int nr = r + dr;
                    if (!on_board(nf, nr)) continue;
                    int target = pos.board[make_sq(nf, nr)];
                    if (target == Empty || (target > 0 ? White : Black) != side) ++mobility;
                }
            }
        }
    }
    return mobility;
}

int phase_blend(int openingScore, int endgameScore, int nonPawnMaterial) {
    int phase = std::clamp(nonPawnMaterial, 0, 6200);
    return (openingScore * phase + endgameScore * (6200 - phase)) / 6200;
}

bool piece_attacks_target(const Position& pos, int from, int target) {
    int piece = pos.board[from];
    if (piece == Empty) return false;
    int side = piece > 0 ? White : Black;
    int type = std::abs(piece);
    int ff = file_of(from);
    int fr = rank_of(from);
    int tf = file_of(target);
    int tr = rank_of(target);
    int df = tf - ff;
    int dr = tr - fr;

    if (type == Pawn) return dr == side && std::abs(df) == 1;
    if (type == Knight) return std::abs(df) * std::abs(dr) == 2;
    if (type == King) return std::max(std::abs(df), std::abs(dr)) == 1;

    bool diagonal = std::abs(df) == std::abs(dr);
    bool straight = df == 0 || dr == 0;
    if (type == Bishop && !diagonal) return false;
    if (type == Rook && !straight) return false;
    if (type == Queen && !diagonal && !straight) return false;

    int stepF = (df > 0) - (df < 0);
    int stepR = (dr > 0) - (dr < 0);
    int f = ff + stepF;
    int r = fr + stepR;
    while (on_board(f, r)) {
        int sq = make_sq(f, r);
        if (sq == target) return true;
        if (pos.board[sq] != Empty) return false;
        f += stepF;
        r += stepR;
    }
    return false;
}

int piece_mobility_from(const Position& pos, int sq, int side) {
    int piece = pos.board[sq];
    int type = std::abs(piece);
    int f = file_of(sq);
    int r = rank_of(sq);
    int mobility = 0;

    if (type == Knight) {
        constexpr int deltas[8][2] = {
            {1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}
        };
        for (auto& d : deltas) {
            int nf = f + d[0];
            int nr = r + d[1];
            if (!on_board(nf, nr)) continue;
            int target = pos.board[make_sq(nf, nr)];
            if (target == Empty || (target > 0 ? White : Black) != side) ++mobility;
        }
    } else if (type == Bishop || type == Rook || type == Queen) {
        static constexpr int dirs[8][2] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
        };
        int begin = type == Bishop ? 4 : 0;
        int end = type == Rook ? 4 : 8;
        for (int i = begin; i < end; ++i) {
            int nf = f + dirs[i][0];
            int nr = r + dirs[i][1];
            while (on_board(nf, nr)) {
                int target = pos.board[make_sq(nf, nr)];
                if (target == Empty) {
                    ++mobility;
                } else {
                    if ((target > 0 ? White : Black) != side) ++mobility;
                    break;
                }
                nf += dirs[i][0];
                nr += dirs[i][1];
            }
        }
    }
    return mobility;
}

int piece_activity_score(const Position& pos, int side, const std::array<std::array<int, 8>, 2>& pawnsByFile,
                         int nonPawnMaterial) {
    int opening = 0;
    int endgame = 0;
    int enemy = -side;
    int enemyKing = pos.king_square(enemy);

    for (int sq = 0; sq < 64; ++sq) {
        int piece = pos.board[sq];
        if (piece == Empty || (piece > 0 ? White : Black) != side) continue;
        int type = std::abs(piece);
        if (type == Pawn || type == King) continue;

        int mob = piece_mobility_from(pos, sq, side);
        if (type == Knight) {
            opening += (mob - 4) * 4;
            endgame += (mob - 4) * 4;
        } else if (type == Bishop) {
            opening += (mob - 6) * 5;
            endgame += (mob - 6) * 5;
        } else if (type == Rook) {
            opening += (mob - 7) * 2;
            endgame += (mob - 7) * 4;

            int file = file_of(sq);
            int color = side == White ? 0 : 1;
            int enemyColor = color ^ 1;
            if (pawnsByFile[color][file] == 0) {
                opening += 8;
                endgame += 10;
                if (pawnsByFile[enemyColor][file] == 0) {
                    opening += 10;
                    endgame += 10;
                }
                if (enemyKing >= 0 && std::abs(file - file_of(enemyKing)) <= 1) opening += 8;
            }
        } else if (type == Queen) {
            opening += (mob - 13);
            endgame += (mob - 13) * 2;
        }

        int relRank = side == White ? rank_of(sq) : 7 - rank_of(sq);
        if ((type == Rook || type == Queen) && relRank == 6 && enemyKing >= 0) {
            int enemyBackRank = side == White ? 7 : 0;
            if (rank_of(enemyKing) == enemyBackRank) {
                opening += type == Rook ? 16 : 8;
                endgame += type == Rook ? 28 : 14;
            }
        }
    }

    return phase_blend(opening, endgame, nonPawnMaterial);
}

int pawn_shelter_penalty(const Position& pos, int side, int kingSq) {
    int penalty = 0;
    int kf = file_of(kingSq);
    int kr = rank_of(kingSq);
    int dir = side == White ? 1 : -1;
    for (int file = std::max(0, kf - 1); file <= std::min(7, kf + 1); ++file) {
        int friendlyDist = 5;
        for (int r = kr + dir; on_board(file, r); r += dir) {
            if (pos.board[make_sq(file, r)] == side * Pawn) {
                friendlyDist = std::abs(r - kr);
                break;
            }
        }
        if (friendlyDist == 1) penalty -= file == kf ? 6 : 3;
        else if (friendlyDist == 2) penalty += file == kf ? 4 : 2;
        else penalty += file == kf ? 16 : 10;

        int stormDist = 5;
        for (int r = kr + dir; on_board(file, r); r += dir) {
            if (pos.board[make_sq(file, r)] == -side * Pawn) {
                stormDist = std::abs(r - kr);
                break;
            }
        }
        if (stormDist <= 2) penalty += 14;
        else if (stormDist == 3) penalty += 7;
    }
    return std::max(0, penalty);
}

int king_attack_pressure(const Position& pos, int side) {
    int kingSq = pos.king_square(side);
    if (kingSq < 0) return 0;
    int attackers = 0;
    int units = 0;
    for (int sq = 0; sq < 64; ++sq) {
        int piece = pos.board[sq];
        if (piece == Empty || (piece > 0 ? White : Black) != -side) continue;
        int type = std::abs(piece);
        if (type == Pawn || type == King) continue;

        bool attacksZone = piece_attacks_target(pos, sq, kingSq);
        for (int df = -1; df <= 1 && !attacksZone; ++df) {
            for (int dr = -1; dr <= 1 && !attacksZone; ++dr) {
                int nf = file_of(kingSq) + df;
                int nr = rank_of(kingSq) + dr;
                if (on_board(nf, nr) && piece_attacks_target(pos, sq, make_sq(nf, nr))) attacksZone = true;
            }
        }
        if (!attacksZone) continue;
        ++attackers;
        if (type == Knight || type == Bishop) units += 1;
        else if (type == Rook) units += 2;
        else if (type == Queen) units += 4;
    }
    if (attackers < 2) return units * 4;
    int weight = std::min(16, attackers + units);
    return units * (8 + weight * 3);
}

int king_safety(const Position& pos, int side) {
    int kingSq = pos.king_square(side);
    if (kingSq < 0) return 0;
    int f = file_of(kingSq);
    int r = rank_of(kingSq);
    int score = 0;

    int homeRank = side == White ? 0 : 7;
    if (r == homeRank && (f <= 2 || f >= 6)) score += 18;
    if (r == homeRank && f >= 3 && f <= 4) score -= 12;

    int shieldRank = r + (side == White ? 1 : -1);
    if (on_board(f, shieldRank)) {
        for (int sf = std::max(0, f - 1); sf <= std::min(7, f + 1); ++sf) {
            if (pos.board[make_sq(sf, shieldRank)] == side * Pawn) score += 8;
        }
    }
    bool flankPawn = false;
    for (int sf = std::max(0, f - 1); sf <= std::min(7, f + 1); ++sf) {
        for (int rr = 0; rr < 8; ++rr) {
            if (pos.board[make_sq(sf, rr)] == side * Pawn) flankPawn = true;
        }
    }
    if (!flankPawn) score -= 16;
    score -= pawn_shelter_penalty(pos, side, kingSq);
    score -= king_attack_pressure(pos, side);

    int enemy = -side;
    for (int df = -1; df <= 1; ++df) {
        for (int dr = -1; dr <= 1; ++dr) {
            if (df == 0 && dr == 0) continue;
            int nf = f + df;
            int nr = r + dr;
            if (on_board(nf, nr) && pos.square_attacked(make_sq(nf, nr), enemy)) score -= 5;
        }
    }
    return score;
}

int king_activity(const Position& pos, int side) {
    int kingSq = pos.king_square(side);
    if (kingSq < 0) return 0;
    int f = file_of(kingSq);
    int r = rank_of(kingSq);
    int centerDistance = std::abs(f - 3) + std::abs(f - 4) + std::abs(r - 3) + std::abs(r - 4);
    return 28 - centerDistance * 4;
}

bool clear_line_between(const Position& pos, int a, int b) {
    int af = file_of(a);
    int ar = rank_of(a);
    int bf = file_of(b);
    int br = rank_of(b);
    int df = (bf > af) - (bf < af);
    int dr = (br > ar) - (br < ar);
    if (af != bf && ar != br && std::abs(af - bf) != std::abs(ar - br)) return false;

    int f = af + df;
    int r = ar + dr;
    while (make_sq(f, r) != b) {
        if (pos.board[make_sq(f, r)] != Empty) return false;
        f += df;
        r += dr;
    }
    return true;
}

int development_score(const Position& pos, int side, int nonPawnMaterial) {
    if (nonPawnMaterial < 5200) return 0;
    int score = 0;
    int home = side == White ? 0 : 7;
    int queenHome = make_sq(3, home);
    int kingHome = make_sq(4, home);
    int color = side;

    if (pos.board[make_sq(1, home)] == color * Knight) score -= 10;
    if (pos.board[make_sq(6, home)] == color * Knight) score -= 10;
    if (pos.board[make_sq(2, home)] == color * Bishop) score -= 8;
    if (pos.board[make_sq(5, home)] == color * Bishop) score -= 8;

    if (pos.board[queenHome] != color * Queen) {
        int undevelopedMinors = 0;
        if (pos.board[make_sq(1, home)] == color * Knight) ++undevelopedMinors;
        if (pos.board[make_sq(6, home)] == color * Knight) ++undevelopedMinors;
        if (pos.board[make_sq(2, home)] == color * Bishop) ++undevelopedMinors;
        if (pos.board[make_sq(5, home)] == color * Bishop) ++undevelopedMinors;
        score -= 6 * undevelopedMinors;
    }

    int kingSq = pos.king_square(side);
    if (kingSq == kingHome) score -= 6;
    return score;
}

int connected_rooks_score(const Position& pos, int side) {
    std::vector<int> rooks;
    for (int sq = 0; sq < 64; ++sq) {
        if (pos.board[sq] == side * Rook) rooks.push_back(sq);
    }
    for (size_t i = 0; i < rooks.size(); ++i) {
        for (size_t j = i + 1; j < rooks.size(); ++j) {
            if ((file_of(rooks[i]) == file_of(rooks[j]) || rank_of(rooks[i]) == rank_of(rooks[j])) &&
                clear_line_between(pos, rooks[i], rooks[j])) {
                return 16;
            }
        }
    }
    return 0;
}

bool pawn_attacks_square(const Position& pos, int sq, int bySide) {
    int f = file_of(sq);
    int r = rank_of(sq);
    if (bySide == White) {
        if (f < 7 && r > 0 && pos.board[sq - 7] == Pawn) return true;
        if (f > 0 && r > 0 && pos.board[sq - 9] == Pawn) return true;
    } else {
        if (f > 0 && r < 7 && pos.board[sq + 7] == -Pawn) return true;
        if (f < 7 && r < 7 && pos.board[sq + 9] == -Pawn) return true;
    }
    return false;
}

int vulnerability_score(const Position& pos, int side) {
    int score = 0;
    for (int sq = 0; sq < 64; ++sq) {
        int piece = pos.board[sq];
        if (piece == Empty || (piece > 0 ? White : Black) != side) continue;
        int type = std::abs(piece);
        if (type == King || type == Pawn) continue;

        bool attacked = pos.square_attacked(sq, -side);
        if (!attacked) continue;
        bool defended = pos.square_attacked(sq, side);
        int value = piece_value(piece);
        if (pawn_attacks_square(pos, sq, -side)) score -= std::min(90, value / 7);
        else if (!defended) score -= std::min(55, value / 12);
        else score -= std::min(24, value / 30);
    }
    return score;
}

int space_score(const Position& pos, int side, int nonPawnMaterial) {
    if (nonPawnMaterial < 5200) return 0;
    int score = 0;
    for (int f = 2; f <= 5; ++f) {
        for (int relRank = 1; relRank <= 3; ++relRank) {
            int r = side == White ? relRank : 7 - relRank;
            int sq = make_sq(f, r);
            if (pos.board[sq] == side * Pawn) continue;
            if (pawn_attacks_square(pos, sq, -side)) continue;
            if (pos.board[sq] == Empty) score += 3;

            for (int back = 1; back <= 3; ++back) {
                int br = r - side * back;
                if (!on_board(f, br)) break;
                if (pos.board[make_sq(f, br)] == side * Pawn) {
                    score += 2;
                    break;
                }
            }
        }
    }
    return score;
}

int fruit_pattern_score(const Position& pos, int side, int nonPawnMaterial) {
    int score = 0;
    int enemyPawn = -side * Pawn;
    int ownBishop = side * Bishop;
    int ownRook = side * Rook;
    int ownKing = side * King;
    int home = side == White ? 0 : 7;
    int sixth = side == White ? 5 : 2;
    int seventh = side == White ? 6 : 1;
    int edgeTrap = phase_blend(70, 55, nonPawnMaterial);
    int nearEdgeTrap = phase_blend(36, 28, nonPawnMaterial);

    if (pos.board[make_sq(0, seventh)] == ownBishop && pos.board[make_sq(1, sixth)] == enemyPawn) score -= edgeTrap;
    if (pos.board[make_sq(7, seventh)] == ownBishop && pos.board[make_sq(6, sixth)] == enemyPawn) score -= edgeTrap;
    if (pos.board[make_sq(0, sixth)] == ownBishop && pos.board[make_sq(1, sixth - side)] == enemyPawn) score -= nearEdgeTrap;
    if (pos.board[make_sq(7, sixth)] == ownBishop && pos.board[make_sq(6, sixth - side)] == enemyPawn) score -= nearEdgeTrap;

    if (pos.board[make_sq(2, home)] == ownBishop &&
        pos.board[make_sq(3, home + side)] == side * Pawn &&
        pos.board[make_sq(3, home + 2 * side)] != Empty) {
        score -= phase_blend(36, 8, nonPawnMaterial);
    }
    if (pos.board[make_sq(5, home)] == ownBishop &&
        pos.board[make_sq(4, home + side)] == side * Pawn &&
        pos.board[make_sq(4, home + 2 * side)] != Empty) {
        score -= phase_blend(36, 8, nonPawnMaterial);
    }

    bool queenSideCastle = pos.board[make_sq(2, home)] == ownKing || pos.board[make_sq(1, home)] == ownKing;
    bool kingSideCastle = pos.board[make_sq(6, home)] == ownKing || pos.board[make_sq(5, home)] == ownKing;
    if (queenSideCastle &&
        (pos.board[make_sq(0, home)] == ownRook || pos.board[make_sq(0, home + side)] == ownRook ||
         pos.board[make_sq(1, home)] == ownRook)) {
        score -= phase_blend(34, 4, nonPawnMaterial);
    }
    if (kingSideCastle &&
        (pos.board[make_sq(7, home)] == ownRook || pos.board[make_sq(7, home + side)] == ownRook ||
         pos.board[make_sq(6, home)] == ownRook)) {
        score -= phase_blend(34, 4, nonPawnMaterial);
    }

    return score;
}

int chebyshev_distance(int a, int b) {
    return std::max(std::abs(file_of(a) - file_of(b)), std::abs(rank_of(a) - rank_of(b)));
}

bool pawn_path_clear(const Position& pos, int sq, int side) {
    int f = file_of(sq);
    for (int r = rank_of(sq) + side; on_board(f, r); r += side) {
        if (pos.board[make_sq(f, r)] != Empty) return false;
    }
    return true;
}

int pawn_promotion_moves(const Position& pos, int sq, int side) {
    int r = rank_of(sq);
    int moves = side == White ? 7 - r : r;
    int homeRank = side == White ? 1 : 6;
    int oneStep = r + side;
    int twoStep = r + 2 * side;
    if (r == homeRank && on_board(file_of(sq), twoStep) &&
        pos.board[make_sq(file_of(sq), oneStep)] == Empty &&
        pos.board[make_sq(file_of(sq), twoStep)] == Empty) {
        --moves;
    }
    return moves;
}

bool pawn_is_passed(const Position& pos, int sq, int side) {
    int f = file_of(sq);
    int r = rank_of(sq);
    for (int af = std::max(0, f - 1); af <= std::min(7, f + 1); ++af) {
        for (int tr = r + side; on_board(af, tr); tr += side) {
            if (pos.board[make_sq(af, tr)] == -side * Pawn) return false;
        }
    }
    return true;
}

bool pawn_is_protected(const Position& pos, int sq, int side) {
    int f = file_of(sq);
    int r = rank_of(sq) - side;
    for (int df : {-1, 1}) {
        int nf = f + df;
        if (on_board(nf, r) && pos.board[make_sq(nf, r)] == side * Pawn) return true;
    }
    return false;
}

int pawn_ending_passed_score(const Position& pos, int sq, int side, int ownKing, int enemyKing) {
    int f = file_of(sq);
    int r = rank_of(sq);
    int relRank = side == White ? r : 7 - r;
    int promotionSq = make_sq(f, side == White ? 7 : 0);
    int oneAhead = make_sq(f, r + side);
    int score = 55 + relRank * relRank * 12;

    bool pathClear = pawn_path_clear(pos, sq, side);
    bool protectedPasser = pawn_is_protected(pos, sq, side);
    if (protectedPasser) score += 55 + relRank * 8;
    if (!pathClear) score -= 45 + relRank * 7;

    int enemyFrontDistance = chebyshev_distance(enemyKing, oneAhead);
    int ownFrontDistance = chebyshev_distance(ownKing, oneAhead);
    score += std::max(0, 42 - ownFrontDistance * 12);
    score -= std::max(0, 36 - enemyFrontDistance * 12);

    int moves = pawn_promotion_moves(pos, sq, side);
    int enemyMoves = chebyshev_distance(enemyKing, promotionSq);
    int enemyTempo = pos.side == -side ? 0 : 1;
    if (pathClear && enemyMoves > moves + enemyTempo) {
        score += 170 + relRank * 35;
    } else if (pathClear && enemyMoves == moves + enemyTempo) {
        score += protectedPasser ? 90 : 25;
    } else if (pathClear) {
        score -= 20;
    }

    int enemyBlockSq = make_sq(f, r + side);
    if (enemyKing == enemyBlockSq && !protectedPasser) score -= 115 + relRank * 15;
    if (ownKing == enemyBlockSq) score += 120 + relRank * 18;

    int nearestEnemyPawn = 8;
    for (int osq = 0; osq < 64; ++osq) {
        if (pos.board[osq] == -side * Pawn) {
            nearestEnemyPawn = std::min(nearestEnemyPawn, std::abs(file_of(osq) - f));
        }
    }
    score += std::min(54, nearestEnemyPawn * 18);
    return score;
}

int pawn_ending_side_score(const Position& pos, int side) {
    int ownKing = pos.king_square(side);
    int enemyKing = pos.king_square(-side);
    if (ownKing < 0 || enemyKing < 0) return 0;

    int score = 0;
    std::array<int, 8> ownFiles{};
    std::array<int, 8> enemyFiles{};
    std::vector<int> pawns;
    for (int sq = 0; sq < 64; ++sq) {
        if (pos.board[sq] == side * Pawn) {
            ++ownFiles[file_of(sq)];
            pawns.push_back(sq);
        } else if (pos.board[sq] == -side * Pawn) {
            ++enemyFiles[file_of(sq)];
        }
    }

    int spareTempi = 0;
    for (int sq : pawns) {
        int f = file_of(sq);
        int r = rank_of(sq);
        int relRank = side == White ? r : 7 - r;
        int aheadRank = r + side;

        score += relRank * 7;
        score += std::max(0, 18 - chebyshev_distance(ownKing, sq) * 5);
        score -= std::max(0, 16 - chebyshev_distance(enemyKing, sq) * 5);

        if (pawn_is_passed(pos, sq, side)) {
            score += pawn_ending_passed_score(pos, sq, side, ownKing, enemyKing);
        } else {
            bool candidate = true;
            for (int af = std::max(0, f - 1); af <= std::min(7, f + 1); ++af) {
                for (int tr = r + side; on_board(af, tr); tr += side) {
                    if (pos.board[make_sq(af, tr)] == -side * Pawn && af == f) candidate = false;
                }
            }
            if (candidate && relRank >= 3) score += 22 + relRank * 5;
        }

        if (on_board(f, aheadRank)) {
            int blocker = pos.board[make_sq(f, aheadRank)];
            if (blocker == side * Pawn) score -= 26;
            if (blocker == -side * Pawn) score -= 34;
            if (make_sq(f, aheadRank) == enemyKing) score -= 48;
        }

        bool opposed = false;
        for (int tr = r + side; on_board(f, tr); tr += side) {
            if (pos.board[make_sq(f, tr)] == -side * Pawn) opposed = true;
        }
        if (!opposed && ownFiles[f] > 0 && enemyFiles[f] == 0) score += 15;
        if (r == (side == White ? 1 : 6)) ++spareTempi;
    }

    int leftMostOwn = 8;
    int rightMostOwn = -1;
    int leftMostEnemy = 8;
    int rightMostEnemy = -1;
    int ownCount = 0;
    int enemyCount = 0;
    for (int f = 0; f < 8; ++f) {
        if (ownFiles[f] > 0) {
            leftMostOwn = std::min(leftMostOwn, f);
            rightMostOwn = std::max(rightMostOwn, f);
            ownCount += ownFiles[f];
        }
        if (enemyFiles[f] > 0) {
            leftMostEnemy = std::min(leftMostEnemy, f);
            rightMostEnemy = std::max(rightMostEnemy, f);
            enemyCount += enemyFiles[f];
        }
    }

    score += (ownCount - enemyCount) * 24;
    score += spareTempi * 9;
    for (int f = 0; f < 8; ++f) {
        if (ownFiles[f] > enemyFiles[f]) score += (ownFiles[f] - enemyFiles[f]) * 11;
        if (ownFiles[f] > 1) score -= (ownFiles[f] - 1) * 14;
    }

    if (ownCount > 0 && enemyCount > 0) {
        int outside = std::max(std::abs(leftMostOwn - leftMostEnemy), std::abs(rightMostOwn - rightMostEnemy));
        score += outside * 10;
    }

    bool sameLineOpposition = file_of(ownKing) == file_of(enemyKing) || rank_of(ownKing) == rank_of(enemyKing);
    int kGap = chebyshev_distance(ownKing, enemyKing);
    if (sameLineOpposition && kGap >= 2) {
        int emptySquaresBetween = kGap - 1;
        if ((emptySquaresBetween & 1) == 1) {
            int oppositionValue = kGap == 2 ? 34 : std::max(10, 28 - kGap * 3);
            score += pos.side == -side ? oppositionValue : -oppositionValue;
        }
    }

    if (sameLineOpposition && ownCount > 0 && enemyCount > 0) {
        score += spareTempi * (pos.side == side ? 6 : 3);
    }

    return score;
}

int pawn_ending_score(const Position& pos) {
    int whitePawns = 0;
    int blackPawns = 0;
    for (int piece : pos.board) {
        if (piece == Pawn) ++whitePawns;
        else if (piece == -Pawn) ++blackPawns;
        else if (piece != Empty && std::abs(piece) != King) return 0;
    }
    if (whitePawns + blackPawns == 0) return 0;
    return pawn_ending_side_score(pos, White) - pawn_ending_side_score(pos, Black);
}

bool pawn_has_phalanx(const Position& pos, int sq, int side) {
    int f = file_of(sq);
    int r = rank_of(sq);
    for (int df : {-1, 1}) {
        int nf = f + df;
        if (on_board(nf, r) && pos.board[make_sq(nf, r)] == side * Pawn) return true;
    }
    return false;
}

int advanced_pawn_structure_score(const Position& pos, int side,
                                  const std::array<std::array<int, 8>, 2>& pawnsByFile) {
    int score = 0;
    int color = side == White ? 0 : 1;
    for (int sq = 0; sq < 64; ++sq) {
        if (pos.board[sq] != side * Pawn) continue;
        int f = file_of(sq);
        int r = rank_of(sq);
        int relRank = side == White ? r : 7 - r;
        bool isolated = (f == 0 || pawnsByFile[color][f - 1] == 0) &&
                        (f == 7 || pawnsByFile[color][f + 1] == 0);
        bool protectedPawn = pawn_is_protected(pos, sq, side);
        bool phalanx = pawn_has_phalanx(pos, sq, side);

        if (protectedPawn || phalanx) {
            score += 4 + relRank * 2;
            if (phalanx) score += 3 + relRank;
        }

        int stopRank = r + side;
        if (!isolated && !protectedPawn && !phalanx && !pawn_is_passed(pos, sq, side) &&
            on_board(f, stopRank) && pos.board[make_sq(f, stopRank)] == Empty) {
            int stopSq = make_sq(f, stopRank);
            if (pawn_attacks_square(pos, stopSq, -side) && !pawn_attacks_square(pos, stopSq, side)) {
                score -= 10 + relRank;
                if (pawnsByFile[color ^ 1][f] == 0) score -= 5;
            }
        }
    }
    return score;
}

int bishop_quality_score(const Position& pos, int side, int bishopCount, int nonPawnMaterial) {
    if (bishopCount == 0) return 0;
    int score = 0;

    bool queensidePawn = false;
    bool kingsidePawn = false;
    for (int sq = 0; sq < 64; ++sq) {
        if (pos.board[sq] != side * Pawn) continue;
        if (file_of(sq) <= 2) queensidePawn = true;
        if (file_of(sq) >= 5) kingsidePawn = true;
    }
    if (queensidePawn && kingsidePawn) score += phase_blend(3, 12, nonPawnMaterial);

    if (bishopCount == 1) {
        int bishopSq = -1;
        for (int sq = 0; sq < 64; ++sq) {
            if (pos.board[sq] == side * Bishop) {
                bishopSq = sq;
                break;
            }
        }
        if (bishopSq >= 0) {
            int bishopColor = (file_of(bishopSq) + rank_of(bishopSq)) & 1;
            int sameColorPawns = 0;
            for (int sq = 0; sq < 64; ++sq) {
                if (pos.board[sq] == side * Pawn && ((file_of(sq) + rank_of(sq)) & 1) == bishopColor) {
                    ++sameColorPawns;
                }
            }
            score -= phase_blend(sameColorPawns * 2, sameColorPawns * 4, nonPawnMaterial);
        }
    }
    return score;
}

int passer_file_support_score(const Position& pos, int sq, int side, int relRank) {
    int f = file_of(sq);
    int score = 0;
    for (int r = rank_of(sq) - side; on_board(f, r); r -= side) {
        int piece = pos.board[make_sq(f, r)];
        if (piece == Empty) continue;
        int type = std::abs(piece);
        if ((piece > 0 ? White : Black) == side) {
            if (type == Rook) score += 16 + relRank * 3;
            else if (type == Queen) score += 10 + relRank * 2;
        } else {
            if (type == Rook) score -= 24 + relRank * 4;
            else if (type == Queen) score -= 14 + relRank * 3;
        }
        break;
    }
    return score;
}

bool enemy_major_controls_square(const Position& pos, int targetSq, int side) {
    for (int sq = 0; sq < 64; ++sq) {
        int piece = pos.board[sq];
        if (piece != -side * Rook && piece != -side * Queen) continue;
        bool sameLine = file_of(sq) == file_of(targetSq) || rank_of(sq) == rank_of(targetSq);
        if (sameLine && clear_line_between(pos, sq, targetSq)) return true;
    }
    return false;
}

int passed_pawn_major_brake_score(const Position& pos, int sq, int side, int relRank, int nonPawnMaterial) {
    if (nonPawnMaterial == 0 || nonPawnMaterial > 2400) return 0;

    int f = file_of(sq);
    int forwardRank = rank_of(sq) + side;
    if (!on_board(f, forwardRank)) return 0;

    int stopSq = make_sq(f, forwardRank);
    int ownKingSq = pos.king_square(side);
    bool protectedByOwnKing = ownKingSq >= 0 && chebyshev_distance(ownKingSq, stopSq) <= 1;
    bool controlledByMajor = enemy_major_controls_square(pos, stopSq, side);

    int score = 0;
    if (controlledByMajor && !protectedByOwnKing) score -= 42 + relRank * 11;

    int enemyKingSq = pos.king_square(-side);
    if (enemyKingSq >= 0) {
        int promotionSq = make_sq(f, side == White ? 7 : 0);
        int kingStopDistance = chebyshev_distance(enemyKingSq, stopSq);
        int kingPromotionDistance = chebyshev_distance(enemyKingSq, promotionSq);
        if (kingStopDistance <= 2 && !protectedByOwnKing) score -= 28 + relRank * 7;
        if (kingPromotionDistance <= 2 && controlledByMajor) score -= 22 + relRank * 6;
    }

    return score;
}

int opposite_bishop_scale(const Position& pos, int score,
                          const std::array<std::array<int, 7>, 2>& pieceCounts) {
    if (score == 0) return score;
    if (pieceCounts[0][Bishop] != 1 || pieceCounts[1][Bishop] != 1) return score;
    for (int type : {Knight, Rook, Queen}) {
        if (pieceCounts[0][type] != 0 || pieceCounts[1][type] != 0) return score;
    }

    int whiteBishop = -1;
    int blackBishop = -1;
    int whitePawns = 0;
    int blackPawns = 0;
    for (int sq = 0; sq < 64; ++sq) {
        if (pos.board[sq] == Bishop) whiteBishop = sq;
        else if (pos.board[sq] == -Bishop) blackBishop = sq;
        else if (pos.board[sq] == Pawn) ++whitePawns;
        else if (pos.board[sq] == -Pawn) ++blackPawns;
    }
    if (whiteBishop < 0 || blackBishop < 0) return score;
    if (((file_of(whiteBishop) + rank_of(whiteBishop) + file_of(blackBishop) + rank_of(blackBishop)) & 1) == 0) {
        return score;
    }

    int pawnDiff = std::abs(whitePawns - blackPawns);
    if (whitePawns < 4 && blackPawns < 4) return score * 55 / 100;
    if (pawnDiff <= 1) return score * 70 / 100;
    return score * 85 / 100;
}

int evaluate(Position& pos) {
    int score = 0;
    int nonPawnMaterial = 0;
    std::array<int, 2> bishops{};
    std::array<std::array<int, 7>, 2> pieceCounts{};
    std::array<std::array<int, 8>, 2> pawnsByFile{};
    for (int sq = 0; sq < 64; ++sq) {
        int p = pos.board[sq];
        if (p == Empty) continue;
        int sign = p > 0 ? White : Black;
        int color = sign == White ? 0 : 1;
        int f = file_of(sq);
        int r = sign == White ? rank_of(sq) : 7 - rank_of(sq);
        int center = 6 - (std::abs(f - 3) + std::abs(f - 4) + std::abs(r - 3) + std::abs(r - 4));
        int pst = 0;
        if (std::abs(p) == Pawn) pst = r * 6 - std::abs(f - 3) * 2;
        else if (std::abs(p) == Knight || std::abs(p) == Bishop) pst = center * 4;
        score += sign * (piece_value(p) + pst);
        if (std::abs(p) == Bishop) ++bishops[color];
        if (std::abs(p) == Pawn) ++pawnsByFile[color][f];
        ++pieceCounts[color][std::abs(p)];
        if (std::abs(p) != Pawn && std::abs(p) != King) nonPawnMaterial += piece_value(p);
    }

    if (bishops[0] >= 2) score += 30;
    if (bishops[1] >= 2) score -= 30;

    for (int file = 0; file < 8; ++file) {
        if (pawnsByFile[0][file] > 1) score -= 12 * (pawnsByFile[0][file] - 1);
        if (pawnsByFile[1][file] > 1) score += 12 * (pawnsByFile[1][file] - 1);
    }

    for (int sq = 0; sq < 64; ++sq) {
        int p = pos.board[sq];
        if (p == Empty) continue;
        int sign = p > 0 ? White : Black;
        int color = sign == White ? 0 : 1;
        int enemy = color ^ 1;
        int f = file_of(sq);
        int r = rank_of(sq);

        if (std::abs(p) == Pawn) {
            bool isolated = (f == 0 || pawnsByFile[color][f - 1] == 0) &&
                            (f == 7 || pawnsByFile[color][f + 1] == 0);
            if (isolated) score -= sign * 10;

            bool passed = true;
            for (int af = std::max(0, f - 1); af <= std::min(7, f + 1); ++af) {
                for (int tsq = af; tsq < 64; tsq += 8) {
                    int target = pos.board[tsq];
                    if (target == -sign * Pawn) {
                        int tr = rank_of(tsq);
                        if ((sign == White && tr > r) || (sign == Black && tr < r)) {
                            passed = false;
                        }
                    }
                }
            }
            if (passed) {
                int relRank = sign == White ? r : 7 - r;
                int bonus = 12 + relRank * relRank * 3;
                int forwardRank = r + sign;
                if (on_board(f, forwardRank) && pos.board[make_sq(f, forwardRank)] != Empty) bonus -= 14 + relRank * 3;

                bool protectedByPawn = false;
                int defenderRank = r - sign;
                for (int df : {-1, 1}) {
                    int nf = f + df;
                    if (on_board(nf, defenderRank) && pos.board[make_sq(nf, defenderRank)] == sign * Pawn) {
                        protectedByPawn = true;
                    }
                }
                if (protectedByPawn) bonus += 10 + relRank * 3;
                bonus += passer_file_support_score(pos, sq, sign, relRank);
                bonus += passed_pawn_major_brake_score(pos, sq, sign, relRank, nonPawnMaterial);

                int enemyKingSq = pos.king_square(-sign);
                if (enemyKingSq >= 0) {
                    int promotionSq = make_sq(f, sign == White ? 7 : 0);
                    int enemyDistance = std::abs(file_of(enemyKingSq) - f) + std::abs(rank_of(enemyKingSq) - rank_of(promotionSq));
                    if (enemyDistance > 7 - relRank) bonus += relRank * 2;
                }
                int ownKingSq = pos.king_square(sign);
                if (ownKingSq >= 0 && nonPawnMaterial <= 2200) {
                    int pawnDistance = std::abs(file_of(ownKingSq) - f) + std::abs(rank_of(ownKingSq) - r);
                    bonus += std::max(0, 12 - pawnDistance * 2);
                }
                score += sign * bonus;
            }
        } else if (std::abs(p) == Rook) {
            if (pawnsByFile[color][f] == 0 && pawnsByFile[enemy][f] == 0) score += sign * 18;
            else if (pawnsByFile[color][f] == 0) score += sign * 8;

            int relRank = sign == White ? r : 7 - r;
            int enemyKingSq = pos.king_square(-sign);
            int enemyKingRank = enemyKingSq >= 0 ? rank_of(enemyKingSq) : -1;
            int enemyBackRank = sign == White ? 7 : 0;
            if (relRank == 6 && enemyKingRank == enemyBackRank) score += sign * 18;
        } else if (std::abs(p) == Knight) {
            int relRank = sign == White ? r : 7 - r;
            bool defendedByPawn = false;
            bool attackedByEnemyPawn = false;
            int defenderRank = r - sign;
            int attackerRank = r + sign;
            for (int df : {-1, 1}) {
                int nf = f + df;
                if (on_board(nf, defenderRank) && pos.board[make_sq(nf, defenderRank)] == sign * Pawn) defendedByPawn = true;
                if (on_board(nf, attackerRank) && pos.board[make_sq(nf, attackerRank)] == -sign * Pawn) attackedByEnemyPawn = true;
            }
            if (relRank >= 3 && defendedByPawn && !attackedByEnemyPawn) score += sign * 20;
        } else if (std::abs(p) == Bishop) {
            int relRank = sign == White ? r : 7 - r;
            bool defendedByPawn = false;
            bool attackedByEnemyPawn = false;
            int defenderRank = r - sign;
            int attackerRank = r + sign;
            for (int df : {-1, 1}) {
                int nf = f + df;
                if (on_board(nf, defenderRank) && pos.board[make_sq(nf, defenderRank)] == sign * Pawn) defendedByPawn = true;
                if (on_board(nf, attackerRank) && pos.board[make_sq(nf, attackerRank)] == -sign * Pawn) attackedByEnemyPawn = true;
            }
            if (relRank >= 3 && defendedByPawn && !attackedByEnemyPawn) score += sign * 12;
        }
    }

    score += advanced_pawn_structure_score(pos, White, pawnsByFile) -
             advanced_pawn_structure_score(pos, Black, pawnsByFile);
    score += bishop_quality_score(pos, White, bishops[0], nonPawnMaterial) -
             bishop_quality_score(pos, Black, bishops[1], nonPawnMaterial);
    score += piece_activity_score(pos, White, pawnsByFile, nonPawnMaterial) -
             piece_activity_score(pos, Black, pawnsByFile, nonPawnMaterial);
    score += development_score(pos, White, nonPawnMaterial) - development_score(pos, Black, nonPawnMaterial);
    score += connected_rooks_score(pos, White) - connected_rooks_score(pos, Black);
    score += vulnerability_score(pos, White) - vulnerability_score(pos, Black);
    score += space_score(pos, White, nonPawnMaterial) - space_score(pos, Black, nonPawnMaterial);
    score += fruit_pattern_score(pos, White, nonPawnMaterial) - fruit_pattern_score(pos, Black, nonPawnMaterial);
    if (nonPawnMaterial <= 2200) {
        score += king_activity(pos, White) - king_activity(pos, Black);
    } else {
        score += king_safety(pos, White) - king_safety(pos, Black);
    }
    if (nonPawnMaterial == 0) score += pawn_ending_score(pos);
    score = opposite_bishop_scale(pos, score, pieceCounts);
    score += pos.side * 8;
    return pos.side * score;
}

class Search {
public:
    explicit Search(int hashMb) : tt(entries_for_hash(hashMb)), ttMask(tt.size() - 1) {}

    void resize_hash(int hashMb) {
        tt.clear();
        tt.resize(entries_for_hash(hashMb));
        ttMask = tt.size() - 1;
    }

    void clear_hash() {
        for (auto& entry : tt) entry = {};
    }

    void clear_heuristics() {
        killers = {};
        history = {};
        captureHistory = {};
    }

    Move best_move(Position& root, int depthLimit, int timeMs, std::uint64_t nodeLimit, int contemptCp,
                   std::atomic_bool* stopFlag, const std::vector<std::uint64_t>& gameHistory) {
        start = Clock::now();
        limitMs = timeMs;
        maxNodes = nodeLimit;
        contempt = contemptCp;
        externalStop = stopFlag;
        stopped = false;
        nodes = 0;
        killers = {};
        if (++ttGeneration == 0) ttGeneration = 1;
        age_heuristics();
        repetitionHistory = gameHistory;
        if (repetitionHistory.empty() || repetitionHistory.back() != hash_key(root)) {
            repetitionHistory.push_back(hash_key(root));
        }
        Move best{};
        int bestScore = -Inf;
        long long lastDepthMs = 1;

        for (int depth = 1; depth <= depthLimit; ++depth) {
            if (limitMs > 0 && depth > 1) {
                long long elapsed = elapsed_ms();
                if (elapsed + std::max(20LL, lastDepthMs * 2 + 30) >= limitMs) break;
            }
            long long depthStart = elapsed_ms();
            Move current{};
            int alpha = -Inf;
            int beta = Inf;
            int score = 0;
            if (depth >= 4 && std::abs(bestScore) < MateScore - 1000) {
                int window = 50;
                alpha = bestScore - window;
                beta = bestScore + window;
                while (true) {
                    score = search_root(root, depth, alpha, beta, current);
                    if (stopped) break;
                    if (score <= alpha) {
                        alpha = std::max(-Inf, alpha - window);
                        window *= 2;
                    } else if (score >= beta) {
                        beta = std::min(Inf, beta + window);
                        window *= 2;
                    } else {
                        break;
                    }
                    if (window > 1200) {
                        alpha = -Inf;
                        beta = Inf;
                    }
                }
            } else {
                score = search_root(root, depth, alpha, beta, current);
            }
            if (stopped) break;
            best = current;
            bestScore = score;
            long long elapsed = elapsed_ms();
            lastDepthMs = std::max(1LL, elapsed - depthStart);
            std::cout << "info depth " << depth << " score cp " << bestScore
                      << " nodes " << nodes << " time " << elapsed
                      << " pv " << principal_variation(root, best, depth) << '\n';
        }
        return best;
    }

    std::uint64_t node_count() const { return nodes; }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int MaxPly = 128;
    enum TTFlag : std::uint8_t { TTExact = 1, TTLower = 2, TTUpper = 3 };

    struct TTEntry {
        std::uint64_t key = 0;
        int depth = -1;
        int score = 0;
        std::uint16_t bestMove = 0;
        std::uint8_t flag = 0;
        std::uint8_t generation = 0;
    };

    struct ScoredMove {
        Move move;
        int score = 0;
    };

    Clock::time_point start;
    int limitMs = 0;
    std::uint64_t maxNodes = 0;
    int contempt = 0;
    bool stopped = false;
    std::atomic_bool* externalStop = nullptr;
    std::uint64_t nodes = 0;
    std::vector<TTEntry> tt;
    std::size_t ttMask = 0;
    std::uint8_t ttGeneration = 1;
    std::array<std::array<std::uint16_t, 2>, MaxPly> killers{};
    std::array<std::array<std::array<int, 64>, 64>, 2> history{};
    std::array<std::array<std::array<std::array<int, 7>, 64>, 7>, 2> captureHistory{};
    std::vector<std::uint64_t> repetitionHistory;
    int probcutStack = 0;

    static std::size_t entries_for_hash(int hashMb) {
        std::size_t bytes = static_cast<std::size_t>(std::max(1, hashMb)) * 1024ULL * 1024ULL;
        std::size_t wanted = std::max<std::size_t>(1, bytes / sizeof(TTEntry));
        std::size_t entries = 1;
        while ((entries << 1) <= wanted) entries <<= 1;
        return entries;
    }

    void age_heuristics() {
        for (auto& color : history) {
            for (auto& from : color) {
                for (int& value : from) value /= 2;
            }
        }
        for (auto& color : captureHistory) {
            for (auto& moved : color) {
                for (auto& to : moved) {
                    for (int& value : to) value /= 2;
                }
            }
        }
    }

    bool out_of_time() {
        if (externalStop && externalStop->load()) return true;
        if (maxNodes > 0 && nodes >= maxNodes) return true;
        if (limitMs <= 0) return false;
        return elapsed_ms() >= limitMs;
    }

    long long elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }

    int search_root(Position& pos, int depth, int alpha, int beta, Move& best) {
        std::uint64_t key = hash_key(pos);
        std::uint16_t ttMove = probe_best_move(pos);
        auto moves = MoveGen::legal_moves(pos);
        order_moves(pos, moves, ttMove, 0);
        if (moves.empty()) return pos.in_check(pos.side) ? -MateScore : 0;
        if (pos.halfmove >= 100 || insufficient_material(pos) || is_repetition(key)) {
            best = moves.front();
            return draw_score();
        }

        int bestScore = -Inf;
        std::uint16_t bestPacked = 0;
        int alphaOrig = alpha;
        int betaOrig = beta;
        int searched = 0;
        for (const Move& move : moves) {
            Undo undo = pos.make_move(move);
            repetitionHistory.push_back(hash_key(pos));
            int score;
            if (searched == 0) {
                score = -negamax(pos, depth - 1, -beta, -alpha, 1);
            } else {
                score = -negamax(pos, depth - 1, -alpha - 1, -alpha, 1);
                if (!stopped && score > alpha && score < beta) {
                    score = -negamax(pos, depth - 1, -beta, -alpha, 1);
                }
            }
            repetitionHistory.pop_back();
            pos.unmake_move(move, undo);
            if (stopped) break;
            ++searched;
            if (score > bestScore) {
                bestScore = score;
                best = move;
                bestPacked = pack_move(move);
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) break;
        }
        if (stopped) return bestScore;
        TTFlag flag = TTExact;
        if (bestScore <= alphaOrig) flag = TTUpper;
        else if (bestScore >= betaOrig) flag = TTLower;
        store_tt(pos, depth, bestScore, flag, bestPacked, 0);
        return bestScore;
    }

    int negamax(Position& pos, int depth, int alpha, int beta, int ply) {
        if (ply >= MaxPly - 1) return evaluate(pos);
        std::uint64_t key = hash_key(pos);
        if (pos.halfmove >= 100 || insufficient_material(pos) || is_repetition(key)) return draw_score();
        if ((nodes++ & 255ULL) == 0 && out_of_time()) {
            stopped = true;
            return alpha;
        }
        bool inCheck = pos.in_check(pos.side);
        if (inCheck) ++depth;
        if (depth <= 0) return quiesce(pos, alpha, beta, ply);
        int staticEval = evaluate(pos);
        if (!inCheck && depth == 1 && staticEval + 260 <= alpha && std::abs(alpha) < MateScore - 1000) {
            int razor = quiesce(pos, alpha, beta, ply);
            if (razor <= alpha) return razor;
        }
        if (!inCheck && depth <= 2 && std::abs(beta) < MateScore - 1000) {
            int margin = depth == 1 ? 180 : 360;
            if (staticEval - margin >= beta) return staticEval;
        }

        int alphaOrig = alpha;
        TTEntry& entry = tt[key & ttMask];
        std::uint16_t ttMove = 0;
        if (entry.key == key) {
            ttMove = entry.bestMove;
            if (entry.depth >= depth) {
                int ttScore = score_from_tt(entry.score, ply);
                if (entry.flag == TTExact) return ttScore;
                if (entry.flag == TTLower && ttScore >= beta) return ttScore;
                if (entry.flag == TTUpper && ttScore <= alpha) return ttScore;
            }
        }

        std::uint16_t probMove = 0;
        if (probcutStack == 0 && try_probcut(pos, depth, beta, ply, staticEval, ttMove, probMove)) {
            store_tt(pos, depth, beta, TTLower, probMove, ply);
            return beta;
        }
        if (stopped) return alpha;

        if (depth >= 3 && !inCheck && staticEval >= beta - 50 && beta < MateScore - 1000 &&
            has_non_pawn_material(pos, pos.side)) {
            int oldEp = pos.ep;
            int oldHalfmove = pos.halfmove;
            std::uint64_t oldKey = pos.key;
            if (has_en_passant_capturer(pos)) pos.key ^= zobrist_ep_key(file_of(pos.ep));
            pos.ep = -1;
            ++pos.halfmove;
            pos.side = -pos.side;
            pos.key ^= zobrist_side_key();
            int reduction = depth >= 6 ? 3 : 2;
            int score = -negamax(pos, depth - 1 - reduction, -beta, -beta + 1, ply + 1);
            pos.side = -pos.side;
            pos.halfmove = oldHalfmove;
            pos.ep = oldEp;
            pos.key = oldKey;
            if (stopped) return alpha;
            if (score >= beta) return beta;
        }

        if (ttMove == 0 && depth >= 5 && !inCheck) {
            int iidScore = negamax(pos, depth - 2, alpha, beta, ply);
            if (stopped) return alpha;
            (void)iidScore;
            ttMove = probe_best_move(pos);
        }

        auto moves = MoveGen::legal_moves(pos);
        if (moves.empty()) {
            if (pos.in_check(pos.side)) return -MateScore + ply;
            return 0;
        }
        order_moves(pos, moves, ttMove, ply);

        int best = -Inf;
        std::uint16_t bestPacked = 0;
        int searched = 0;
        for (const Move& move : moves) {
            bool quiet = is_quiet(pos, move);
            Undo undo = pos.make_move(move);
            bool givesCheck = pos.in_check(pos.side);
            if (!inCheck && !givesCheck && quiet && depth <= 2 && searched > 0 && staticEval + 180 * depth <= alpha) {
                pos.unmake_move(move, undo);
                continue;
            }
            repetitionHistory.push_back(hash_key(pos));
            int score;
            int childDepth = depth - 1;
            if (searched == 0) {
                score = -negamax(pos, childDepth, -beta, -alpha, ply + 1);
            } else {
                bool reduce = depth >= 3 && searched >= 4 && quiet && !inCheck;
                int searchDepth = reduce ? childDepth - 1 : childDepth;
                score = -negamax(pos, searchDepth, -alpha - 1, -alpha, ply + 1);
                if (reduce && !stopped && score > alpha) {
                    score = -negamax(pos, childDepth, -alpha - 1, -alpha, ply + 1);
                }
                if (!stopped && score > alpha && score < beta) {
                    score = -negamax(pos, childDepth, -beta, -alpha, ply + 1);
                }
            }
            repetitionHistory.pop_back();
            pos.unmake_move(move, undo);
            if (stopped) return alpha;
            ++searched;
            if (score > best) {
                best = score;
                bestPacked = pack_move(move);
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                if (is_quiet(pos, move)) {
                    remember_killer(move, ply);
                    int bonus = depth * depth;
                    history[side_index(pos.side)][move.from][move.to] += bonus;
                } else {
                    remember_capture(pos, move, depth);
                }
                store_tt(pos, depth, best, TTLower, bestPacked, ply);
                break;
            }
        }
        if (alpha < beta) {
            TTFlag flag = best <= alphaOrig ? TTUpper : TTExact;
            store_tt(pos, depth, best, flag, bestPacked, ply);
        }
        return best;
    }

    bool try_probcut(Position& pos, int depth, int beta, int ply, int staticEval,
                     std::uint16_t ttMove, std::uint16_t& moveOut) {
        if (depth < 5 || beta <= -MateScore + 1000 || beta >= MateScore - 1000) return false;
        if (pos.in_check(pos.side)) return false;
        if (staticEval + 260 < beta) return false;

        const int probBeta = beta + 170;
        if (probBeta >= MateScore - 1000) return false;
        const int probDepth = std::max(1, depth - 4);

        auto moves = MoveGen::legal_captures(pos);
        if (moves.empty()) return false;
        order_moves(pos, moves, ttMove, ply);

        ++probcutStack;
        int tried = 0;
        for (const Move& move : moves) {
            int captured = pos.board[move.to];
            if (move.enPassant) captured = -pos.side * Pawn;

            if (move.promotion != Empty && move.promotion != Queen) continue;
            if (captured == Empty && move.promotion == Empty) continue;
            if (move.promotion == Empty && see(pos, move) < 0) continue;
            if (++tried > 8) break;

            Undo undo = pos.make_move(move);
            repetitionHistory.push_back(hash_key(pos));
            int score = -negamax(pos, probDepth, -probBeta, -probBeta + 1, ply + 1);
            repetitionHistory.pop_back();
            pos.unmake_move(move, undo);

            if (stopped) {
                --probcutStack;
                return false;
            }
            if (score >= probBeta) {
                moveOut = pack_move(move);
                --probcutStack;
                return true;
            }
        }
        --probcutStack;
        return false;
    }

    int quiesce(Position& pos, int alpha, int beta, int ply) {
        if (ply >= MaxPly - 1) return evaluate(pos);
        std::uint64_t key = hash_key(pos);
        if (pos.halfmove >= 100 || insufficient_material(pos) || is_repetition(key)) return draw_score();
        if ((nodes++ & 255ULL) == 0 && out_of_time()) {
            stopped = true;
            return alpha;
        }

        bool inCheck = pos.in_check(pos.side);
        int standPat = -Inf;
        if (!inCheck) {
            standPat = evaluate(pos);
            if (standPat >= beta) return beta;
            if (standPat > alpha) alpha = standPat;
        }

        auto moves = inCheck ? MoveGen::legal_moves(pos) : MoveGen::legal_captures(pos);
        if (inCheck && moves.empty()) return -MateScore + ply;
        order_moves(pos, moves, 0, ply);
        for (const Move& move : moves) {
            if (!inCheck && move.promotion == Empty) {
                int captured = pos.board[move.to];
                if (move.enPassant) captured = -pos.side * Pawn;
                int swing = captured == Empty ? 0 : piece_value(captured);
                if (alpha > -MateScore + 1000 && standPat + swing + 180 < alpha) continue;
                if (is_likely_bad_capture(pos, move) && standPat + swing + 360 < alpha) continue;
            }
            Undo undo = pos.make_move(move);
            repetitionHistory.push_back(hash_key(pos));
            int score = -quiesce(pos, -beta, -alpha, ply + 1);
            repetitionHistory.pop_back();
            pos.unmake_move(move, undo);
            if (stopped) return alpha;
            if (score >= beta) return beta;
            if (score > alpha) alpha = score;
        }
        return alpha;
    }

    static int side_index(int side) { return side == White ? 0 : 1; }

    static bool is_quiet(const Position& pos, const Move& move) {
        return pos.board[move.to] == Empty && !move.enPassant && move.promotion == Empty;
    }

    static bool has_non_pawn_material(const Position& pos, int side) {
        for (int piece : pos.board) {
            if (piece != Empty && (piece > 0 ? White : Black) == side) {
                int type = std::abs(piece);
                if (type != Pawn && type != King) return true;
            }
        }
        return false;
    }

    static bool attacks_after_move(const Position& pos, int from, int target, int piece, int vacated, int epCapture) {
        int side = piece > 0 ? White : Black;
        int type = std::abs(piece);
        int ff = file_of(from);
        int fr = rank_of(from);
        int tf = file_of(target);
        int tr = rank_of(target);
        int df = tf - ff;
        int dr = tr - fr;

        if (type == Pawn) return dr == side && std::abs(df) == 1;
        if (type == Knight) return std::abs(df) * std::abs(dr) == 2;
        if (type == King) return std::max(std::abs(df), std::abs(dr)) == 1;

        bool diagonal = std::abs(df) == std::abs(dr);
        bool straight = df == 0 || dr == 0;
        if (type == Bishop && !diagonal) return false;
        if (type == Rook && !straight) return false;
        if (type == Queen && !diagonal && !straight) return false;

        int stepF = (df > 0) - (df < 0);
        int stepR = (dr > 0) - (dr < 0);
        int f = ff + stepF;
        int r = fr + stepR;
        while (on_board(f, r)) {
            int sq = make_sq(f, r);
            if (sq == target) return true;
            if (sq != vacated && sq != epCapture && pos.board[sq] != Empty) return false;
            f += stepF;
            r += stepR;
        }
        return false;
    }

    static bool discovered_check_after_move(const Position& pos, const Move& move, int enemyKing, int epCapture) {
        int kf = file_of(enemyKing);
        int kr = rank_of(enemyKing);
        int ff = file_of(move.from);
        int fr = rank_of(move.from);
        int df = ff - kf;
        int dr = fr - kr;
        bool diagonal = std::abs(df) == std::abs(dr);
        bool straight = df == 0 || dr == 0;
        if (!diagonal && !straight) return false;

        int stepF = (df > 0) - (df < 0);
        int stepR = (dr > 0) - (dr < 0);
        int f = kf + stepF;
        int r = kr + stepR;
        while (on_board(f, r)) {
            int sq = make_sq(f, r);
            if (sq == move.to) return false;
            if (sq == move.from || sq == epCapture) {
                f += stepF;
                r += stepR;
                continue;
            }
            int piece = pos.board[sq];
            if (piece == Empty) {
                f += stepF;
                r += stepR;
                continue;
            }
            if ((piece > 0 ? White : Black) != pos.side) return false;
            int type = std::abs(piece);
            if (diagonal) return type == Bishop || type == Queen;
            return type == Rook || type == Queen;
        }
        return false;
    }

    static bool gives_direct_check_fast(const Position& pos, const Move& move) {
        int enemyKing = pos.king_square(-pos.side);
        if (enemyKing < 0) return false;
        int moved = pos.board[move.from];
        int placed = move.promotion != Empty ? pos.side * move.promotion : moved;
        int epCapture = move.enPassant ? move.to - pos.side * 8 : -1;
        if (attacks_after_move(pos, move.to, enemyKing, placed, move.from, epCapture)) return true;
        return discovered_check_after_move(pos, move, enemyKing, epCapture);
    }

    static bool is_likely_bad_capture(const Position& pos, const Move& move) {
        int captured = pos.board[move.to];
        if (move.enPassant) captured = -pos.side * Pawn;
        if (captured == Empty || move.promotion != Empty || gives_direct_check_fast(pos, move)) return false;

        return see(pos, move) < -40;
    }

    static int least_valuable_attacker(int target, int side, std::array<int, 64>& board) {
        int bestSq = -1;
        int bestValue = Inf;
        for (int sq = 0; sq < 64; ++sq) {
            int piece = board[sq];
            if (piece == Empty || (piece > 0 ? White : Black) != side) continue;
            if (!piece_attacks_square(board, sq, target, piece)) continue;
            int value = piece_value(piece);
            if (value < bestValue) {
                bestValue = value;
                bestSq = sq;
            }
        }
        return bestSq;
    }

    static bool piece_attacks_square(const std::array<int, 64>& board, int from, int target, int piece) {
        int side = piece > 0 ? White : Black;
        int type = std::abs(piece);
        int ff = file_of(from);
        int fr = rank_of(from);
        int tf = file_of(target);
        int tr = rank_of(target);
        int df = tf - ff;
        int dr = tr - fr;

        if (type == Pawn) return dr == side && std::abs(df) == 1;
        if (type == Knight) return std::abs(df) * std::abs(dr) == 2;
        if (type == King) return std::max(std::abs(df), std::abs(dr)) == 1;

        bool diagonal = std::abs(df) == std::abs(dr);
        bool straight = df == 0 || dr == 0;
        if (type == Bishop && !diagonal) return false;
        if (type == Rook && !straight) return false;
        if (type == Queen && !diagonal && !straight) return false;
        if (!diagonal && !straight) return false;

        int stepF = (df > 0) - (df < 0);
        int stepR = (dr > 0) - (dr < 0);
        int f = ff + stepF;
        int r = fr + stepR;
        while (on_board(f, r)) {
            int sq = make_sq(f, r);
            if (sq == target) return true;
            if (board[sq] != Empty) return false;
            f += stepF;
            r += stepR;
        }
        return false;
    }

    static int see(const Position& pos, const Move& move) {
        int captured = pos.board[move.to];
        if (move.enPassant) captured = -pos.side * Pawn;
        if (captured == Empty) return 0;

        std::array<int, 64> board = pos.board;
        int side = pos.side;
        int target = move.to;
        int movingPiece = board[move.from];
        int placedPiece = move.promotion != Empty ? side * move.promotion : movingPiece;
        std::array<int, 32> gain{};
        int depth = 0;

        gain[depth] = piece_value(captured);
        if (move.promotion != Empty) {
            gain[depth] += piece_value(move.promotion) - piece_value(Pawn);
        }
        board[move.from] = Empty;
        if (move.enPassant) board[target - side * 8] = Empty;
        board[target] = placedPiece;
        side = -side;

        int lastCaptured = std::abs(placedPiece);
        while (depth + 1 < static_cast<int>(gain.size())) {
            int attackerSq = least_valuable_attacker(target, side, board);
            if (attackerSq < 0) break;
            ++depth;
            gain[depth] = piece_value(lastCaptured) - gain[depth - 1];
            int attacker = board[attackerSq];
            board[attackerSq] = Empty;
            board[target] = attacker;
            lastCaptured = std::abs(attacker);
            side = -side;
        }

        while (depth > 0) {
            --depth;
            gain[depth] = -std::max(-gain[depth], gain[depth + 1]);
        }
        return gain[0];
    }

    static int score_to_tt(int score, int ply) {
        if (score > MateScore - 1000) return score + ply;
        if (score < -MateScore + 1000) return score - ply;
        return score;
    }

    static int score_from_tt(int score, int ply) {
        if (score > MateScore - 1000) return score - ply;
        if (score < -MateScore + 1000) return score + ply;
        return score;
    }

    std::uint16_t probe_best_move(const Position& pos) const {
        std::uint64_t key = hash_key(pos);
        const TTEntry& entry = tt[key & ttMask];
        return entry.key == key ? entry.bestMove : 0;
    }

    void store_tt(const Position& pos, int depth, int score, TTFlag flag, std::uint16_t bestMove, int ply) {
        std::uint64_t key = hash_key(pos);
        TTEntry& entry = tt[key & ttMask];
        bool replace = entry.key != key || depth >= entry.depth || flag == TTExact ||
                       (bestMove != 0 && entry.bestMove == 0) || entry.generation != ttGeneration;
        if (replace) {
            bool sameKey = entry.key == key;
            entry.key = key;
            entry.depth = depth;
            entry.score = score_to_tt(score, ply);
            entry.flag = flag;
            entry.generation = ttGeneration;
            if (bestMove != 0 || !sameKey) entry.bestMove = bestMove;
        }
    }

    void remember_killer(const Move& move, int ply) {
        if (ply >= MaxPly) return;
        std::uint16_t packed = pack_move(move);
        if (killers[ply][0] != packed) {
            killers[ply][1] = killers[ply][0];
            killers[ply][0] = packed;
        }
    }

    void remember_capture(const Position& pos, const Move& move, int depth) {
        int captured = pos.board[move.to];
        if (move.enPassant) captured = -pos.side * Pawn;
        if (captured == Empty) return;

        int movedType = std::abs(pos.board[move.from]);
        int victimType = std::abs(captured);
        if (movedType <= 0 || movedType > King || victimType <= 0 || victimType > King) return;

        int& entry = captureHistory[side_index(pos.side)][movedType][move.to][victimType];
        int bonus = std::min(1024, 16 * depth * depth);
        entry += bonus - entry * std::abs(bonus) / 16384;
        entry = std::clamp(entry, -16384, 16384);
    }

    bool is_repetition(std::uint64_t key) const {
        int seen = 0;
        for (auto it = repetitionHistory.rbegin(); it != repetitionHistory.rend(); ++it) {
            if (*it == key && ++seen >= 3) return true;
        }
        return false;
    }

    int draw_score() const {
        return -contempt;
    }

    int move_score(const Position& pos, const Move& move, std::uint16_t ttMove, int ply) const {
        if (matches_packed_move(move, ttMove)) return 2'000'000;
        int captured = pos.board[move.to];
        if (move.enPassant) captured = -pos.side * Pawn;
        int moved = pos.board[move.from];
        int score = 0;
        if (captured != Empty) {
            int seeScore = see(pos, move);
            score += piece_value(captured) * 10 - piece_value(moved) + seeScore;
            int movedType = std::abs(moved);
            int victimType = std::abs(captured);
            if (movedType > 0 && movedType <= King && victimType > 0 && victimType <= King) {
                score += captureHistory[side_index(pos.side)][movedType][move.to][victimType] / 16;
            }
            if (seeScore < -40 && move.promotion == Empty && !gives_direct_check_fast(pos, move)) score -= 8'000;
        }
        if (move.promotion != Empty) score += piece_value(move.promotion);
        if (gives_direct_check_fast(pos, move)) score += 6'000;
        if (move.castle) score += 30;
        if (is_quiet(pos, move)) {
            std::uint16_t packed = pack_move(move);
            if (ply < MaxPly && killers[ply][0] == packed) score += 90'000;
            else if (ply < MaxPly && killers[ply][1] == packed) score += 80'000;
            score += history[side_index(pos.side)][move.from][move.to];
        }
        return score;
    }

    void order_moves(const Position& pos, MoveList& moves, std::uint16_t ttMove, int ply) const {
        std::array<ScoredMove, 256> scored{};
        size_t count = std::min(scored.size(), moves.size());
        for (size_t i = 0; i < count; ++i) {
            scored[i] = ScoredMove{moves[i], move_score(pos, moves[i], ttMove, ply)};
        }

        std::sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(count), [](const ScoredMove& a, const ScoredMove& b) {
            return a.score > b.score;
        });
        for (size_t i = 0; i < count; ++i) {
            moves[i] = scored[i].move;
        }
    }

    std::string principal_variation(const Position& root, const Move& first, int maxDepth) const {
        Position line = root;
        Move move = first;
        std::string out;
        std::vector<std::uint64_t> seen;

        for (int ply = 0; ply < maxDepth; ++ply) {
            auto legal = MoveGen::legal_moves(line);
            auto it = std::find_if(legal.begin(), legal.end(), [&](const Move& candidate) {
                return candidate == move;
            });
            if (it == legal.end()) break;

            if (!out.empty()) out.push_back(' ');
            out += move_to_uci(*it);

            Undo undo = line.make_move(*it);
            (void)undo;
            std::uint64_t key = hash_key(line);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) break;
            seen.push_back(key);

            std::uint16_t packed = probe_best_move(line);
            if (packed == 0) break;
            auto nextLegal = MoveGen::legal_moves(line);
            auto next = std::find_if(nextLegal.begin(), nextLegal.end(), [&](const Move& candidate) {
                return matches_packed_move(candidate, packed);
            });
            if (next == nextLegal.end()) break;
            move = *next;
        }

        return out.empty() ? move_to_uci(first) : out;
    }
};

std::optional<Move> parse_move(Position& pos, const std::string& token) {
    if (token.size() < 4) return std::nullopt;
    auto from = parse_square(token.substr(0, 2));
    auto to = parse_square(token.substr(2, 2));
    if (!from || !to) return std::nullopt;
    int promo = Empty;
    if (token.size() >= 5) {
        switch (std::tolower(static_cast<unsigned char>(token[4]))) {
        case 'q': promo = Queen; break;
        case 'r': promo = Rook; break;
        case 'b': promo = Bishop; break;
        case 'n': promo = Knight; break;
        default: return std::nullopt;
        }
    }

    auto moves = MoveGen::legal_moves(pos);
    for (const Move& move : moves) {
        if (move.from == *from && move.to == *to && move.promotion == promo) return move;
    }
    return std::nullopt;
}

std::uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;
    auto moves = MoveGen::legal_moves(pos);
    if (depth == 1) return moves.size();
    std::uint64_t nodes = 0;
    for (const Move& move : moves) {
        Undo undo = pos.make_move(move);
        nodes += perft(pos, depth - 1);
        pos.unmake_move(move, undo);
    }
    return nodes;
}

void perft_divide(Position& pos, int depth) {
    auto moves = MoveGen::legal_moves(pos);
    std::uint64_t total = 0;
    for (const Move& move : moves) {
        Undo undo = pos.make_move(move);
        std::uint64_t n = perft(pos, depth - 1);
        pos.unmake_move(move, undo);
        total += n;
        std::cout << move_to_uci(move) << ": " << n << '\n';
    }
    std::cout << "nodes " << total << '\n';
}

class Uci {
public:
    Uci() : searcher(std::make_unique<Search>(hashMb)) {}

    void loop() {
        pos.set_startpos();
        positionHistory = {hash_key(pos)};
        std::string line;
        while (std::getline(std::cin, line)) {
            handle(line);
            if (quit) break;
        }
        stop_search();
    }

private:
    Position pos;
    bool quit = false;
    int hashMb = 64;
    int moveOverheadMs = 30;
    int contemptCp = 0;
    std::unique_ptr<Search> searcher;
    std::vector<std::uint64_t> positionHistory;
    std::thread searchThread;
    std::atomic_bool stopSearch{false};
    std::mutex outputMutex;

    void handle(const std::string& line) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "uci") {
            std::cout << "id name Crepusculo 0.25\n";
            std::cout << "id author Codex\n";
            std::cout << "option name Hash type spin default 64 min 1 max 1024\n";
            std::cout << "option name Clear Hash type button\n";
            std::cout << "option name Move Overhead type spin default 30 min 0 max 5000\n";
            std::cout << "option name Contempt type spin default 0 min -100 max 100\n";
            std::cout << "uciok\n" << std::flush;
        } else if (cmd == "isready") {
            std::cout << "readyok\n" << std::flush;
        } else if (cmd == "setoption") {
            stop_search();
            set_option(in);
        } else if (cmd == "ucinewgame") {
            stop_search();
            searcher->clear_hash();
            searcher->clear_heuristics();
            pos.set_startpos();
            positionHistory = {hash_key(pos)};
        } else if (cmd == "position") {
            stop_search();
            set_position(in);
        } else if (cmd == "go") {
            stop_search();
            go(in);
        } else if (cmd == "stop") {
            stop_search();
        } else if (cmd == "quit") {
            stop_search();
            quit = true;
        } else if (cmd == "perft") {
            int depth = 1;
            in >> depth;
            perft_divide(pos, depth);
        } else if (cmd == "bench") {
            stop_search();
            bench();
        } else if (cmd == "eval") {
            int stmScore = evaluate(pos);
            int whiteScore = pos.side * stmScore;
            std::cout << "static eval cp " << stmScore << " stm"
                      << " white_cp " << whiteScore << '\n' << std::flush;
        } else if (cmd == "d") {
            print_board();
        }
    }

    void stop_search() {
        stopSearch.store(true);
        if (searchThread.joinable()) searchThread.join();
        stopSearch.store(false);
    }

    void set_option(std::istringstream& in) {
        std::string token;
        std::string name;
        std::string value;
        while (in >> token) {
            if (token == "name") {
                name.clear();
                while (in >> token && token != "value") {
                    if (!name.empty()) name.push_back(' ');
                    name += token;
                }
                if (token != "value") break;
                std::getline(in, value);
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                    value.erase(value.begin());
                }
                break;
            }
        }

        if (name == "Hash" && !value.empty()) {
            hashMb = std::clamp(std::stoi(value), 1, 1024);
            searcher->resize_hash(hashMb);
        } else if (name == "Clear Hash") {
            searcher->clear_hash();
        } else if (name == "Move Overhead" && !value.empty()) {
            moveOverheadMs = std::clamp(std::stoi(value), 0, 5000);
        } else if (name == "Contempt" && !value.empty()) {
            contemptCp = std::clamp(std::stoi(value), -100, 100);
        }
    }

    void set_position(std::istringstream& in) {
        std::string token;
        in >> token;
        if (token == "startpos") {
            pos.set_startpos();
            positionHistory = {hash_key(pos)};
            in >> token;
        } else if (token == "fen") {
            std::string fen;
            std::vector<std::string> parts;
            while (in >> token && token != "moves") parts.push_back(token);
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) fen.push_back(' ');
                fen += parts[i];
            }
            if (!pos.set_fen(fen)) pos.set_startpos();
            positionHistory = {hash_key(pos)};
        }

        if (token != "moves") return;
        while (in >> token) {
            auto move = parse_move(pos, token);
            if (!move) break;
            pos.make_move(*move);
            positionHistory.push_back(hash_key(pos));
        }
    }

    void go(std::istringstream& in) {
        int depth = 5;
        int movetime = 0;
        int wtime = 0;
        int btime = 0;
        int winc = 0;
        int binc = 0;
        int movestogo = 0;
        std::uint64_t nodesLimit = 0;
        bool infinite = false;
        bool depthSpecified = false;
        std::string token;
        while (in >> token) {
            if (token == "depth") {
                in >> depth;
                depthSpecified = true;
            }
            else if (token == "movetime") in >> movetime;
            else if (token == "wtime") in >> wtime;
            else if (token == "btime") in >> btime;
            else if (token == "winc") in >> winc;
            else if (token == "binc") in >> binc;
            else if (token == "movestogo") in >> movestogo;
            else if (token == "nodes") in >> nodesLimit;
            else if (token == "infinite") {
                infinite = true;
                depth = 64;
            }
        }

        int timeMs = 0;
        if (movetime > 0) {
            timeMs = std::max(1, movetime - moveOverheadMs);
        } else if (!infinite && (wtime > 0 || btime > 0)) {
            timeMs = choose_time(wtime, btime, winc, binc, movestogo);
        }
        if (!depthSpecified && timeMs > 0) depth = 64;
        if (!depthSpecified && nodesLimit > 0) depth = 64;
        if (timeMs > 0) depth = std::min(depth, 64);

        auto moves = MoveGen::legal_moves(pos);
        if (moves.empty()) {
            std::cout << "bestmove 0000\n" << std::flush;
            return;
        }
        if (moves.size() == 1) {
            std::cout << "bestmove " << move_to_uci(moves.front()) << '\n' << std::flush;
            return;
        }

        Position root = pos;
        auto rootHistory = positionHistory;
        stopSearch.store(false);
        searchThread = std::thread([this, root, rootHistory, depth, timeMs, nodesLimit, contempt = contemptCp, fallback = moves.front()]() mutable {
            Move best = searcher->best_move(root, depth, timeMs, nodesLimit, contempt, &stopSearch, rootHistory);
            if (best.from == best.to && best.promotion == Empty) best = fallback;
            std::lock_guard<std::mutex> lock(outputMutex);
            std::cout << "bestmove " << move_to_uci(best) << '\n' << std::flush;
        });
    }

    int choose_time(int wtime, int btime, int winc, int binc, int movestogo) const {
        int clock = pos.side == White ? wtime : btime;
        int inc = pos.side == White ? winc : binc;
        int safeClock = std::max(1, clock - moveOverheadMs);

        if (safeClock <= 1000) return std::max(1, safeClock / 20);
        if (safeClock <= 5000) return std::max(10, safeClock / 20);

        int movesLeft = movestogo > 0 ? movestogo : std::clamp(42 - pos.fullmove / 2, 18, 42);
        int base = safeClock * 3 / (2 * movesLeft) + inc * 2 / 3;
        int cap = std::max(20, safeClock / 4);
        int floor = safeClock > 30000 ? 80 : 20;
        return std::clamp(base, floor, cap);
    }

    void bench() {
        static const std::array<std::string, 5> fens = {
            StartFen,
            "r3k2r/p1ppqpb1/bn2pnp1/2pPN3/1p2P3/2N2Q1P/PPPBBPP1/R3K2R w KQkq - 0 1",
            "4rrk1/ppp2ppp/2n2n2/3qp3/3P4/2P1PN2/PPQ2PPP/R1B2RK1 w - - 0 12",
            "8/2p5/3p4/1P1Pp3/4Pp2/5P2/8/4K1k1 w - - 0 1",
            "2r2rk1/pp3ppp/2n1bn2/2qp4/3N4/2PBPN2/PP3PPP/R1BQ1RK1 w - - 0 11",
        };

        Search benchSearch(hashMb);
        std::atomic_bool stop{false};
        std::uint64_t totalNodes = 0;
        auto start = std::chrono::steady_clock::now();
        for (const std::string& fen : fens) {
            Position benchPos;
            benchPos.set_fen(fen);
            std::vector<std::uint64_t> history = {hash_key(benchPos)};
            Move best = benchSearch.best_move(benchPos, 4, 0, 0, contemptCp, &stop, history);
            totalNodes += benchSearch.node_count();
            std::cout << "bench " << fen << " bestmove " << move_to_uci(best)
                      << " nodes " << benchSearch.node_count() << '\n';
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::uint64_t nps = elapsed > 0 ? totalNodes * 1000ULL / static_cast<std::uint64_t>(elapsed) : totalNodes;
        std::cout << "bench nodes " << totalNodes << " time " << elapsed << " nps " << nps << '\n';
    }

    void print_board() const {
        for (int r = 7; r >= 0; --r) {
            std::cout << r + 1 << "  ";
            for (int f = 0; f < 8; ++f) {
                std::cout << piece_to_char(pos.board[make_sq(f, r)]) << ' ';
            }
            std::cout << '\n';
        }
        std::cout << "\n   a b c d e f g h\n";
        std::cout << "side " << (pos.side == White ? "w" : "b") << '\n';
    }
};

} // namespace crepusculo

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::cout << std::unitbuf;
    crepusculo::Uci uci;
    uci.loop();
    return 0;
}
