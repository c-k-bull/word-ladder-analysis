// human_solver.cpp
// Simulates a PERSON solving word ladders with simple rules, and compares rule sets.
//
// Build:  g++ -O2 -std=c++17 human_solver.cpp -o human_solver
// Usage:  ./human_solver common_nouns_3_to_5.csv --dict common_words_3_to_5.csv [options]
//
// ---------------------------------------------------------------------------------------------
// THE SIMULATED PERSON
//   - Looks only one step ahead: the words reachable by changing one letter of the current word.
//   - Remembers only the ladder written so far, which options they already tried from each word,
//     and (with --memory deadends, the default) words that led nowhere.
//   - Never reuses a word already in the ladder.
//   - When no option is left, erases the last word and tries something else (a backtrack).
//   - Gives up after --budget moves (each word written or erased is one move) or after
//     --backtracks erasures.
//   - Picks among equally good options at random, like a person would.
//   - Optionally knows only part of the vocabulary (--vocab).
//
// RULE LETTERS (combine, e.g. "GVP"). "Target" = the end word, or in B mode, the other ladder's tip.
//   G  Make progress      prefer changing a letter to its target letter; otherwise a sideways
//                         change (wrong -> different wrong); breaking a correct letter last
//   V  Vowel pattern      progress: fix vowel positions first;
//                         otherwise: prefer changes that make the consonant/vowel pattern match
//   E  Ends first         progress: fix the first/last letter first
//   P  Protect vowels     never change a vowel that already matches the target (unless nothing else)
//   O  Outer detours      when not making progress, change the first/last letter
//   L  Common bridges     when not making progress, use common letters (b d s f c t ...) over rare
//   B  Both ends          build from the start AND the end at once, alternating, each side aiming
//                         at the other side's latest word; solved when they meet
//   "none" = random choices (with the same memory and backtracking) as a baseline.
//
// OPTIONS
//   --dict FILE          Words allowed as in-between steps (default: input words only)
//   --sample N           Random connected pairs per word length (default 500)
//   --trials T           Attempts per pair per rule set, with different random tie-breaks (default 3)
//   --seed N             Random seed (default 1)
//   --lengths 3,4,5      Word lengths to run (default: all)
//   --strategies LIST    Comma list (default below); --all-combos runs every combination
//   --budget N           Max moves before giving up (default 60)
//   --backtracks N       Max erasures before giving up (default 15)
//   --memory MODE        path | deadends (default deadends)
//   --vocab F            Fraction of dictionary words the person knows, 0-1 (default 1.0)
//   --out-runs FILE      Default human_runs.csv
//   --out-summary FILE   Default human_summary.csv
// ---------------------------------------------------------------------------------------------

#include <algorithm>
#include <climits>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

// ------------------------------------ input & graph ------------------------------------
static string clean(string s) {
    while (!s.empty() && (isspace((unsigned char)s.back()) || s.back() == '"')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (isspace((unsigned char)s[i]) || s[i] == '"')) ++i;
    s = s.substr(i);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool allAlpha(const string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < 'a' || c > 'z') return false;
    return true;
}
static vector<string> readWords(const string& path) {
    ifstream in(path);
    if (!in) { cerr << "Cannot open " << path << "\n"; exit(1); }
    vector<string> words;
    string line;
    bool first = true;
    while (getline(in, line)) {
        string w = clean(line.substr(0, line.find(',')));
        bool header = first && w == "word" && line.find(',') != string::npos;
        first = false;
        if (header || !allAlpha(w)) continue;
        words.push_back(w);
    }
    return words;
}

struct Graph {
    vector<string> words;
    unordered_map<string, int> index;
    vector<vector<int>> adj;
    vector<unordered_set<int>> adjSet;
    int add(const string& w) {
        auto it = index.find(w);
        if (it != index.end()) return it->second;
        index[w] = (int)words.size();
        words.push_back(w);
        return (int)words.size() - 1;
    }
    void build() {
        adj.assign(words.size(), {});
        for (int i = 0; i < (int)words.size(); ++i) {
            string w = words[i];
            for (size_t p = 0; p < w.size(); ++p) {
                char o = w[p];
                for (char c = 'a'; c <= 'z'; ++c) {
                    if (c == o) continue;
                    w[p] = c;
                    auto it = index.find(w);
                    if (it != index.end()) adj[i].push_back(it->second);
                }
                w[p] = o;
            }
        }
        adjSet.resize(words.size());
        for (size_t i = 0; i < words.size(); ++i) adjSet[i] = unordered_set<int>(adj[i].begin(), adj[i].end());
    }
};

static int shortestDistance(const Graph& g, int src, int dst) {
    vector<int> dist(g.words.size(), -1);
    queue<int> q;
    dist[src] = 0;
    q.push(src);
    while (!q.empty()) {
        int u = q.front(); q.pop();
        for (int v : g.adj[u]) {
            if (dist[v] != -1) continue;
            dist[v] = dist[u] + 1;
            if (v == dst) return dist[v];
            q.push(v);
        }
    }
    return -1;
}

// ------------------------------------ rules ------------------------------------
enum : int { R_G = 1, R_V = 2, R_E = 4, R_P = 8, R_O = 16, R_L = 32, R_B = 64 };
static const string RULE_LETTERS = "GVEPOLB";

static int parseMask(const string& s) {
    if (s == "none") return 0;
    int m = 0;
    for (char c : s) {
        size_t k = RULE_LETTERS.find((char)toupper((unsigned char)c));
        if (k == string::npos) { cerr << "Unknown rule letter '" << c << "' in " << s << "\n"; exit(1); }
        m |= 1 << k;
    }
    return m;
}
static string maskName(int m) {
    if (!m) return "none";
    string s;
    for (int k = 0; k < 7; ++k) if (m & (1 << k)) s += RULE_LETTERS[k];
    return s;
}

static bool isVowelAt(const string& w, size_t i) {
    char c = w[i];
    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') return true;
    if (c == 'y') {
        if (i == 0) return false;
        if (i + 1 < w.size()) {
            char d = w[i + 1];
            if (d == 'a' || d == 'e' || d == 'i' || d == 'o' || d == 'u') return false;
        }
        return true;
    }
    return false;
}
static int cvMismatch(const string& a, const string& b) {
    int m = 0;
    for (size_t i = 0; i < a.size(); ++i) m += isVowelAt(a, i) != isVowelAt(b, i);
    return m;
}

static double RARITY[26];
static void initRarity() {
    const string order = "bdsfctpkhmnlrwyaigozuevjxq";   // most to least useful bridge letter
    for (size_t r = 0; r < order.size(); ++r) RARITY[order[r] - 'a'] = (double)r / (order.size() - 1);
}

// Sort key for moving from word u to neighbor v while aiming at target t. Smaller = preferred.
using Key = tuple<int, int, double, int, int, double, double>;
static Key moveKey(const string& u, const string& v, const string& t, int mask, mt19937_64& rng) {
    size_t n = u.size(), i = 0;
    while (u[i] == v[i]) ++i;                       // the changed position
    bool wasRight = u[i] == t[i], nowRight = v[i] == t[i];
    bool outer = (i == 0 || i == n - 1);
    int cat = nowRight ? 0 : (wasRight ? 2 : 1);     // 0 progress, 1 sideways, 2 breaks a correct letter

    int pViolate = (mask & R_P) && wasRight && isVowelAt(u, i) ? 1 : 0;
    int catKey = (mask & R_G) ? cat : 0;
    double vKey = 0, lKey = 0;
    int eKey = 0, oKey = 0;
    if (cat == 0) {
        if (mask & R_V) vKey = isVowelAt(t, i) ? 0 : 1;
        if (mask & R_E) eKey = outer ? 0 : 1;
    } else {
        if (mask & R_V) vKey = cvMismatch(v, t) - cvMismatch(u, t);
        if (mask & R_O) oKey = outer ? 0 : 1;
        if (mask & R_L) lKey = RARITY[v[i] - 'a'];
    }
    double r = uniform_real_distribution<double>(0, 1)(rng);
    return {pViolate, catKey, vKey, eKey, oKey, lKey, r};
}

// ------------------------------------ one attempt ------------------------------------
struct Attempt {
    bool solved = false;
    int pathLen = -1, moves = 0, backtracks = 0;
    string outcome;   // solved | budget | backtrack_limit | stuck
    vector<int> path;
};

struct Side {
    vector<int> stack;
    vector<unordered_set<int>> tried;   // options already tried from each stack position
    bool stuck = false;
    int tip() const { return stack.back(); }
};

static Attempt solve(const Graph& g, int src, int dst, int mask, const vector<char>& known,
                     bool deadEndMemory, int budget, int maxBacktracks, mt19937_64& rng) {
    Attempt a;
    size_t N = g.words.size();
    vector<char> onPath(N, 0), dead(N, 0);
    bool both = mask & R_B;

    Side F, Bk;
    F.stack = {src};
    F.tried.emplace_back();
    onPath[src] = 1;
    if (both) {
        Bk.stack = {dst};
        Bk.tried.emplace_back();
        onPath[dst] = 1;
    }

    auto met = [&]() -> bool {
        if (!both) return F.tip() == dst;
        return F.tip() == Bk.tip() || g.adjSet[F.tip()].count(Bk.tip());
    };
    auto finish = [&]() {
        a.solved = true;
        a.outcome = "solved";
        a.path = F.stack;
        if (both)
            for (auto it = Bk.stack.rbegin(); it != Bk.stack.rend(); ++it)
                if (*it != a.path.back()) a.path.push_back(*it);
        a.pathLen = (int)a.path.size() - 1;
    };

    // One move for one side: write a new word, or erase the last one.
    auto step = [&](Side& s, int target) {
        int u = s.tip();
        const string& uw = g.words[u];
        const string& tw = g.words[target];
        int best = -1;
        Key bestKey;
        for (int v : g.adj[u]) {
            bool isGoal = (!both && v == dst);
            if (!isGoal && (!known[v] || onPath[v] || dead[v])) continue;
            if (s.tried.back().count(v)) continue;
            Key k = moveKey(uw, g.words[v], tw, mask, rng);
            if (best == -1 || k < bestKey) { best = v; bestKey = k; }
        }
        ++a.moves;
        if (best != -1) {
            s.tried.back().insert(best);
            s.stack.push_back(best);
            s.tried.emplace_back();
            onPath[best] = 1;
            return;
        }
        if (s.stack.size() == 1) { s.stuck = true; --a.moves; return; }   // nothing to erase
        onPath[u] = 0;
        if (deadEndMemory) dead[u] = 1;
        s.stack.pop_back();
        s.tried.pop_back();
        ++a.backtracks;
    };

    bool forwardTurn = true;
    while (true) {
        if (met()) { finish(); return a; }
        if (a.moves >= budget) { a.outcome = "budget"; return a; }
        if (a.backtracks > maxBacktracks) { a.outcome = "backtrack_limit"; return a; }
        if (F.stuck && (!both || Bk.stuck)) { a.outcome = "stuck"; return a; }

        if (!both) step(F, dst);
        else {
            bool useF = forwardTurn ? !F.stuck : Bk.stuck;
            if (useF) step(F, Bk.tip());
            else step(Bk, F.tip());
            forwardTurn = !forwardTurn;
        }
    }
}

// ------------------------------------ summary ------------------------------------
struct Agg {
    long long runs = 0, solved = 0, solvedShortest = 0, budget = 0, btLimit = 0, stuck = 0;
    double extra = 0, moves = 0, backtracks = 0;
    void add(const Attempt& a, int shortest) {
        ++runs;
        moves += a.moves;
        backtracks += a.backtracks;
        if (a.outcome == "budget") ++budget;
        else if (a.outcome == "backtrack_limit") ++btLimit;
        else if (a.outcome == "stuck") ++stuck;
        if (!a.solved) return;
        ++solved;
        extra += a.pathLen - shortest;
        if (a.pathLen == shortest) ++solvedShortest;
    }
};

static string joinPath(const Graph& g, const vector<int>& p) {
    string s;
    for (size_t i = 0; i < p.size(); ++i) { if (i) s += '>'; s += g.words[p[i]]; }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " input.csv [--dict FILE] [--sample N] [--trials T] [--seed N]"
                " [--lengths 3,4,5] [--strategies LIST | --all-combos] [--budget N] [--backtracks N]"
                " [--memory path|deadends] [--vocab F] [--out-runs FILE] [--out-summary FILE]\n";
        return 1;
    }
    initRarity();
    string inputPath = argv[1], dictPath, outRuns = "human_runs.csv", outSummary = "human_summary.csv";
    string strategyList = "none,G,GV,GE,GP,GO,GL,GPO,GPOL,GVP,GVPO,GVPOL,GB,GPB,GPOB,GVPOB,GVPOLB";
    bool allCombos = false, deadEndMemory = true;
    size_t sample = 500;
    int trials = 3, budget = 60, maxBacktracks = 15;
    double vocab = 1.0;
    uint64_t seed = 1;
    set<size_t> lengths;
    for (int i = 2; i < argc; ++i) {
        string a = argv[i];
        auto next = [&]() -> string {
            if (i + 1 >= argc) { cerr << "Missing value for " << a << "\n"; exit(1); }
            return argv[++i];
        };
        if (a == "--dict") dictPath = next();
        else if (a == "--sample") sample = stoull(next());
        else if (a == "--trials") trials = max(1, stoi(next()));
        else if (a == "--seed") seed = stoull(next());
        else if (a == "--strategies") strategyList = next();
        else if (a == "--all-combos") allCombos = true;
        else if (a == "--budget") budget = stoi(next());
        else if (a == "--backtracks") maxBacktracks = stoi(next());
        else if (a == "--vocab") vocab = stod(next());
        else if (a == "--memory") {
            string m = next();
            if (m == "path") deadEndMemory = false;
            else if (m == "deadends") deadEndMemory = true;
            else { cerr << "--memory must be path or deadends\n"; return 1; }
        }
        else if (a == "--out-runs") outRuns = next();
        else if (a == "--out-summary") outSummary = next();
        else if (a == "--lengths") {
            stringstream ss(next());
            string x;
            while (getline(ss, x, ',')) lengths.insert(stoul(x));
        } else { cerr << "Unknown option " << a << "\n"; return 1; }
    }

    vector<int> masks;
    if (allCombos) for (int m = 0; m < 128; ++m) masks.push_back(m);
    else {
        stringstream ss(strategyList);
        string x;
        while (getline(ss, x, ',')) masks.push_back(parseMask(clean(x)));
    }

    map<size_t, vector<string>> byLen;
    {
        set<string> seen;
        for (auto& w : readWords(inputPath))
            if (seen.insert(w).second && (lengths.empty() || lengths.count(w.size())))
                byLen[w.size()].push_back(w);
    }
    map<size_t, Graph> graphs;
    for (auto& [len, ws] : byLen) for (auto& w : ws) graphs[len].add(w);
    if (!dictPath.empty())
        for (auto& w : readWords(dictPath))
            if (graphs.count(w.size())) graphs[w.size()].add(w);
    for (auto& [len, g] : graphs) g.build();

    ofstream fr(outRuns), fs(outSummary);
    if (!fr || !fs) { cerr << "Cannot open output files\n"; return 1; }
    fr << "length,pair_id,start,end,shortest,strategy,trial,solved,outcome,path_len,extra_steps,"
          "moves,backtracks,path\n";

    mt19937_64 rng(seed);
    map<pair<string, string>, Agg> agg;   // (length or "all", strategy)
    long long pairId = 0;

    for (auto& [len, ws] : byLen) {
        Graph& g = graphs[len];
        vector<tuple<int, int, int>> pairs;
        set<pair<int, int>> used;
        uniform_int_distribution<size_t> pick(0, ws.size() - 1);
        size_t attempts = 0;
        while (pairs.size() < sample && attempts++ < 200 * sample + 1000) {
            size_t i = pick(rng), j = pick(rng);
            if (i == j) continue;
            int a = g.index.at(ws[i]), b = g.index.at(ws[j]);
            if (!used.insert({min(a, b), max(a, b)}).second) continue;
            int d = shortestDistance(g, a, b);
            if (d > 0) pairs.emplace_back(a, b, d);
        }
        cerr << len << "-letter: " << pairs.size() << " pairs x " << masks.size() << " rule sets x "
             << trials << " trials\n";

        size_t done = 0;
        for (auto& [src, dst, shortest] : pairs) {
            ++pairId;
            for (int trial = 1; trial <= trials; ++trial) {
                // the person's vocabulary for this attempt (start and end are always known)
                vector<char> known(g.words.size(), 1);
                if (vocab < 1.0) {
                    bernoulli_distribution knows(vocab);
                    for (auto& k : known) k = knows(rng);
                }
                known[src] = known[dst] = 1;
                for (int mask : masks) {
                    Attempt a = solve(g, src, dst, mask, known, deadEndMemory, budget, maxBacktracks, rng);
                    string name = maskName(mask);
                    fr << len << "," << pairId << "," << g.words[src] << "," << g.words[dst] << ","
                       << shortest << "," << name << "," << trial << "," << (a.solved ? 1 : 0) << ","
                       << a.outcome << "," << a.pathLen << ","
                       << (a.solved ? to_string(a.pathLen - shortest) : string("")) << ","
                       << a.moves << "," << a.backtracks << "," << joinPath(g, a.path) << "\n";
                    agg[{to_string(len), name}].add(a, shortest);
                    agg[{"all", name}].add(a, shortest);
                }
            }
            if (++done % 25 == 0 || done == pairs.size())
                cerr << "\r  " << done << "/" << pairs.size() << " pairs done"
                     << (done == pairs.size() ? "\n" : "") << flush;
        }
    }

    fs << "length,strategy,runs,pct_solved,pct_solved_with_shortest,mean_extra_steps_when_solved,"
          "mean_moves,mean_backtracks,pct_out_of_moves,pct_too_many_backtracks,pct_stuck\n";
    fs.setf(ios::fixed);
    fs.precision(2);
    for (auto& [key, a] : agg) {
        double r = (double)a.runs, s = a.solved ? (double)a.solved : 1;
        fs << key.first << "," << key.second << "," << a.runs << "," << 100 * a.solved / r << ","
           << 100 * a.solvedShortest / r << "," << a.extra / s << "," << a.moves / r << ","
           << a.backtracks / r << "," << 100 * a.budget / r << "," << 100 * a.btLimit / r << ","
           << 100 * a.stuck / r << "\n";
    }
    cerr << "Wrote " << outRuns << " and " << outSummary << "\n";
    return 0;
}
