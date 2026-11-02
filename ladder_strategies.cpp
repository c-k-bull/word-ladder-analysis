// ladder_strategies.cpp
// Benchmarks word-ladder search strategies: two base algorithms, each run plain and guided by
// strategy combinations built from the move analysis. Times every search and checks whether the
// FIRST path each one finds is a shortest path.
//
// Build:  g++ -O2 -std=c++17 ladder_strategies.cpp -o ladder_strategies
// Usage:  ./ladder_strategies common_nouns_3_to_5.csv --dict common_words_3_to_5.csv [options]
//
// ---------------------------------------------------------------------------------------------
// BASE ALGORITHMS
//   bfs  First-hit search. Expands one word at a time, tries every position with letters a-z,
//        skips any word already seen, and stops the instant the end word is generated.
//        With no strategy it expands words first-in-first-out (plain breadth-first).
//        With a strategy it expands the word the strategy scores best next (best-first).
//   bnb  Branch and bound (depth-first). Follows one path as deep as it can, records the first
//        complete ladder, then keeps searching for shorter ones. A branch is cut when
//        (steps so far + letters still wrong) >= best ladder found so far, or when the word was
//        already reached in as few steps. Finishes with a guaranteed shortest ladder unless
//        the --bnb-limit expansion cap is hit.
//        With no strategy it tries neighbors in a-z order; with a strategy, best-scored first.
//   astar A* search. Expands words in order of (steps taken + letters still wrong). Because
//        "letters still wrong" never overestimates the steps left, the first ladder it finds is
//        always a shortest one. A strategy only breaks ties between words with equal totals,
//        so strategies change how much work A* does, never the ladder length.
//
// STRATEGIES (combine letters, e.g. "GVP"; lower score = tried first)
//   G  Greedy progress       letters that differ from the end word
//   V  Vowels first          consonant/vowel pattern mismatches + end-word vowels not yet placed
//   E  Ends first            first/last letters not yet matching the end word
//   P  Protect vowels        vowels that match in start AND end but are currently changed
//   O  Outer bridges         temporary letters sitting in middle positions (push them outward)
//   L  Common bridges        rarity of temporary letters (b d s f c t... preferred over x q j)
//   "none" = the plain base algorithm.
//   A "temporary letter" is one matching neither the start nor end word at that position.
//
// OPTIONS
//   --dict FILE          Words allowed as in-between steps (default: input words only)
//   --sample N           Random connected pairs per word length (default 500; 0 = every pair)
//   --seed N             Seed for sampling (default 1)
//   --lengths 3,4,5      Word lengths to run (default: all in the input)
//   --strategies LIST    Comma list (default: none,G,V,E,P,O,L,GV,GP,GVP,GVPO,GVPOL,GVEPOL)
//   --all-combos         Run all 64 combinations instead
//   --algorithms LIST    Comma list of bfs,bnb,astar (default: all three)
//   --bnb-limit N        Max expansions per branch-and-bound search (default 2000000)
//   --repeat R           Time each search R times and keep the fastest (default 1)
//   --out-runs FILE      Default strategy_runs.csv     (one row per pair x algorithm x strategy)
//   --out-summary FILE   Default strategy_summary.csv  (averages per length x algorithm x strategy)
// ---------------------------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;

// ------------------------------------ input ------------------------------------
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

// ------------------------------------ graph ------------------------------------
struct Graph {
    vector<string> words;
    unordered_map<string, int> index;
    vector<vector<int>> adj;   // neighbors in (position, then a-z) order
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
    }
};

// ------------------------------------ strategies ------------------------------------
enum : int { F_G = 1, F_V = 2, F_E = 4, F_P = 8, F_O = 16, F_L = 32 };
static const string FEATURE_LETTERS = "GVEPOL";

static int parseMask(const string& s) {
    if (s == "none") return 0;
    int m = 0;
    for (char c : s) {
        size_t k = FEATURE_LETTERS.find((char)toupper((unsigned char)c));
        if (k == string::npos) { cerr << "Unknown strategy letter '" << c << "' in " << s << "\n"; exit(1); }
        m |= 1 << k;
    }
    return m;
}
static string maskName(int m) {
    if (!m) return "none";
    string s;
    for (int k = 0; k < 6; ++k) if (m & (1 << k)) s += FEATURE_LETTERS[k];
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

// Bridge-letter preference from the analysis (most over-used as a bridge -> least).
static double RARITY[26];
static void initRarity() {
    const string order = "bdsfctpkhmnlrwyaigozuevjxq";
    for (size_t r = 0; r < order.size(); ++r) RARITY[order[r] - 'a'] = (double)r / (order.size() - 1);
}

// Everything a score needs about one start/end pair, precomputed once.
struct Target {
    string start, end;
    size_t n;
    vector<char> endVowel, protectedVowel;
    Target(const string& s, const string& e) : start(s), end(e), n(e.size()) {
        endVowel.resize(n);
        protectedVowel.resize(n);
        for (size_t i = 0; i < n; ++i) {
            endVowel[i] = isVowelAt(end, i);
            protectedVowel[i] = (start[i] == end[i]) && endVowel[i];
        }
    }
    int hamming(const string& w) const {
        int h = 0;
        for (size_t i = 0; i < n; ++i) h += w[i] != end[i];
        return h;
    }
    double score(const string& w, int mask) const {
        double s = 0;
        for (size_t i = 0; i < n; ++i) {
            bool wrong = w[i] != end[i];
            bool temp = wrong && w[i] != start[i];
            if ((mask & F_G) && wrong) s += 1;
            if (mask & F_V) {
                if (isVowelAt(w, i) != (bool)endVowel[i]) s += 1;
                if (endVowel[i] && wrong) s += 1;
            }
            if ((mask & F_E) && wrong && (i == 0 || i == n - 1)) s += 1;
            if ((mask & F_P) && protectedVowel[i] && wrong) s += 1;
            if ((mask & F_O) && temp && i != 0 && i != n - 1) s += 1;
            if ((mask & F_L) && temp) s += RARITY[w[i] - 'a'];
        }
        return s;
    }
};

// ------------------------------------ searches ------------------------------------
struct Result {
    bool found = false;
    int firstLen = -1;     // steps in the first ladder found
    int finalLen = -1;     // steps in the ladder returned at the end (bnb improves; bfs = first)
    long long expansions = 0;
    bool hitLimit = false;
    vector<int> firstPath, finalPath;
};

// Plain BFS distance, used as the ground-truth shortest length.
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

static vector<int> tracePath(const vector<int>& parent, int dst) {
    vector<int> p;
    for (int v = dst; v != -1; v = parent[v]) p.push_back(v);
    reverse(p.begin(), p.end());
    return p;
}

// Base 1: first-hit search. mask = 0 -> FIFO (breadth-first); otherwise best-first by score.
static Result firstHit(const Graph& g, int src, int dst, const Target& t, int mask) {
    Result r;
    size_t N = g.words.size();
    vector<int> parent(N, -1);
    vector<char> seen(N, 0);
    seen[src] = 1;

    auto finish = [&](int v) {
        r.found = true;
        r.firstPath = r.finalPath = tracePath(parent, v);
        r.firstLen = r.finalLen = (int)r.firstPath.size() - 1;
    };

    if (mask == 0) {
        queue<int> q;
        q.push(src);
        while (!q.empty()) {
            int u = q.front(); q.pop();
            ++r.expansions;
            for (int v : g.adj[u]) {
                if (seen[v]) continue;
                seen[v] = 1;
                parent[v] = u;
                if (v == dst) { finish(v); return r; }
                q.push(v);
            }
        }
        return r;
    }

    // (score, insertion order, node): ties fall back to first-in-first-out
    using Item = tuple<double, long long, int>;
    priority_queue<Item, vector<Item>, greater<Item>> pq;
    long long order = 0;
    pq.emplace(t.score(g.words[src], mask), order++, src);
    while (!pq.empty()) {
        int u = get<2>(pq.top()); pq.pop();
        ++r.expansions;
        for (int v : g.adj[u]) {
            if (seen[v]) continue;
            seen[v] = 1;
            parent[v] = u;
            if (v == dst) { finish(v); return r; }
            pq.emplace(t.score(g.words[v], mask), order++, v);
        }
    }
    return r;
}

// A*: priority = steps so far + letters still wrong (never overestimates, so results are
// shortest). Ties: strategy score (if any), then first-in-first-out.
// Stopping when the end word is generated is safe here: every expanded word u has
// steps(u) + wrong(u) <= shortest, and wrong(u) >= 1, so steps(u) + 1 <= shortest.
static Result aStar(const Graph& g, int src, int dst, const Target& t, int mask) {
    Result r;
    size_t N = g.words.size();
    vector<int> parent(N, -1), gScore(N, INT_MAX);
    vector<char> closed(N, 0);
    using Item = tuple<int, double, long long, int>;   // f, tie score, order, node
    priority_queue<Item, vector<Item>, greater<Item>> pq;
    long long order = 0;
    auto push = [&](int v, int gv) {
        double tie = mask ? t.score(g.words[v], mask) : 0.0;
        pq.emplace(gv + t.hamming(g.words[v]), tie, order++, v);
    };
    gScore[src] = 0;
    push(src, 0);
    while (!pq.empty()) {
        int u = get<3>(pq.top()); pq.pop();
        if (closed[u]) continue;            // stale entry
        closed[u] = 1;
        ++r.expansions;
        for (int v : g.adj[u]) {
            int gv = gScore[u] + 1;
            if (gv >= gScore[v]) continue;
            gScore[v] = gv;
            parent[v] = u;
            if (v == dst) {
                r.found = true;
                r.firstPath = r.finalPath = tracePath(parent, v);
                r.firstLen = r.finalLen = (int)r.firstPath.size() - 1;
                return r;
            }
            push(v, gv);
        }
    }
    return r;
}

// Base 2: depth-first branch and bound.
static Result branchAndBound(const Graph& g, int src, int dst, const Target& t, int mask,
                             long long limit) {
    Result r;
    size_t N = g.words.size();
    vector<int> bestDepth(N, INT_MAX);
    vector<char> onPath(N, 0);
    vector<int> path{src};
    int best = INT_MAX;
    bestDepth[src] = 0;
    onPath[src] = 1;

    function<void(int, int)> dfs = [&](int u, int depth) {
        if (r.hitLimit) return;
        if (++r.expansions > limit) { r.hitLimit = true; return; }

        // children in a-z order, or sorted by strategy score (stable, so a-z breaks ties)
        vector<int> kids = g.adj[u];
        if (mask) {
            vector<pair<double, int>> scored;
            scored.reserve(kids.size());
            for (int v : kids) scored.emplace_back(t.score(g.words[v], mask), v);
            stable_sort(scored.begin(), scored.end(),
                        [](const pair<double, int>& a, const pair<double, int>& b) { return a.first < b.first; });
            for (size_t k = 0; k < kids.size(); ++k) kids[k] = scored[k].second;
        }

        for (int v : kids) {
            int d = depth + 1;
            if (v == dst) {
                path.push_back(v);
                if (!r.found) { r.found = true; r.firstLen = d; r.firstPath = path; }
                if (d < best) { best = d; r.finalLen = d; r.finalPath = path; }
                path.pop_back();
                continue;
            }
            if (onPath[v]) continue;
            if (d + t.hamming(g.words[v]) >= best) continue;   // bound: can't beat best
            if (bestDepth[v] <= d) continue;                   // reached here as fast before
            bestDepth[v] = d;
            onPath[v] = 1;
            path.push_back(v);
            dfs(v, d);
            path.pop_back();
            onPath[v] = 0;
            if (r.hitLimit) return;
        }
    };
    dfs(src, 0);
    return r;
}

static string joinPath(const Graph& g, const vector<int>& p) {
    string s;
    for (size_t i = 0; i < p.size(); ++i) { if (i) s += '>'; s += g.words[p[i]]; }
    return s;
}

// ------------------------------------ summary ------------------------------------
struct Agg {
    long long runs = 0, found = 0, firstShortest = 0, finalShortest = 0, limitHits = 0;
    double extraFirst = 0, extraFinal = 0, timeUs = 0, expansions = 0;
    void add(const Result& r, int shortest, double us) {
        ++runs;
        timeUs += us;
        expansions += (double)r.expansions;
        if (r.hitLimit) ++limitHits;
        if (!r.found) return;
        ++found;
        if (r.firstLen == shortest) ++firstShortest;
        if (r.finalLen == shortest) ++finalShortest;
        extraFirst += r.firstLen - shortest;
        extraFinal += r.finalLen - shortest;
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " input.csv [--dict FILE] [--sample N] [--seed N] [--lengths 3,4,5]"
                " [--strategies LIST | --all-combos] [--algorithms bfs,bnb,astar] [--bnb-limit N] [--repeat R]"
                " [--out-runs FILE] [--out-summary FILE]\n";
        return 1;
    }
    initRarity();
    string inputPath = argv[1], dictPath, outRuns = "strategy_runs.csv", outSummary = "strategy_summary.csv";
    string strategyList = "none,G,V,E,P,O,L,GV,GP,GVP,GVPO,GVPOL,GVEPOL";
    bool allCombos = false;
    vector<string> algorithms{"bfs", "bnb", "astar"};
    size_t sample = 500;
    uint64_t seed = 1;
    long long bnbLimit = 2000000;
    int repeat = 1;
    set<size_t> lengths;
    for (int i = 2; i < argc; ++i) {
        string a = argv[i];
        auto next = [&]() -> string {
            if (i + 1 >= argc) { cerr << "Missing value for " << a << "\n"; exit(1); }
            return argv[++i];
        };
        if (a == "--dict") dictPath = next();
        else if (a == "--sample") sample = stoull(next());
        else if (a == "--seed") seed = stoull(next());
        else if (a == "--strategies") strategyList = next();
        else if (a == "--all-combos") allCombos = true;
        else if (a == "--algorithms") {
            algorithms.clear();
            stringstream ss(next());
            string x;
            while (getline(ss, x, ',')) {
                x = clean(x);
                if (x != "bfs" && x != "bnb" && x != "astar") { cerr << "Unknown algorithm " << x << "\n"; return 1; }
                algorithms.push_back(x);
            }
        }
        else if (a == "--bnb-limit") bnbLimit = stoll(next());
        else if (a == "--repeat") repeat = max(1, stoi(next()));
        else if (a == "--out-runs") outRuns = next();
        else if (a == "--out-summary") outSummary = next();
        else if (a == "--lengths") {
            stringstream ss(next());
            string x;
            while (getline(ss, x, ',')) lengths.insert(stoul(x));
        } else { cerr << "Unknown option " << a << "\n"; return 1; }
    }

    vector<int> masks;
    if (allCombos) for (int m = 0; m < 64; ++m) masks.push_back(m);
    else {
        stringstream ss(strategyList);
        string x;
        while (getline(ss, x, ',')) masks.push_back(parseMask(clean(x)));
    }

    // words to pair (input) and graphs (input + dictionary)
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
    fr << "length,pair_id,start,end,shortest,algorithm,strategy,found,first_len,first_is_shortest,"
          "final_len,final_is_shortest,expansions,time_us,hit_limit,first_path\n";

    mt19937_64 rng(seed);
    map<tuple<string, string, string>, Agg> agg;   // (length or "all", algorithm, strategy)
    long long pairId = 0;

    for (auto& [len, ws] : byLen) {
        Graph& g = graphs[len];

        // pick pairs (connected only), with their true shortest length
        vector<tuple<int, int, int>> pairs;   // src, dst, shortest
        if (sample == 0) {
            for (size_t i = 0; i < ws.size(); ++i)
                for (size_t j = i + 1; j < ws.size(); ++j) {
                    int a = g.index.at(ws[i]), b = g.index.at(ws[j]);
                    int d = shortestDistance(g, a, b);
                    if (d > 0) pairs.emplace_back(a, b, d);
                }
        } else {
            set<pair<int, int>> used;
            uniform_int_distribution<size_t> pick(0, ws.size() - 1);
            size_t attempts = 0, maxAttempts = 200 * sample + 1000;
            while (pairs.size() < sample && attempts++ < maxAttempts) {
                size_t i = pick(rng), j = pick(rng);
                if (i == j) continue;
                int a = g.index.at(ws[i]), b = g.index.at(ws[j]);
                if (!used.insert({min(a, b), max(a, b)}).second) continue;
                int d = shortestDistance(g, a, b);
                if (d > 0) pairs.emplace_back(a, b, d);
            }
            if (pairs.size() < sample)
                cerr << "Note: only found " << pairs.size() << " connected " << len << "-letter pairs\n";
        }
        cerr << len << "-letter: " << pairs.size() << " pairs x " << masks.size()
             << " strategies x " << algorithms.size() << " algorithms\n";

        size_t done = 0;
        auto lenStart = Clock::now();
        for (auto& [src, dst, shortest] : pairs) {
            ++pairId;
            if (++done % 10 == 0 || done == pairs.size()) {
                double secs = chrono::duration<double>(Clock::now() - lenStart).count();
                cerr << "\r  " << done << "/" << pairs.size() << " pairs done ("
                     << (int)secs << "s)" << (done == pairs.size() ? "\n" : "") << flush;
            }
            Target t(g.words[src], g.words[dst]);
            for (const string& algoName : algorithms) {
                for (int mask : masks) {
                    Result r;
                    double bestUs = 1e18;
                    for (int rep = 0; rep < repeat; ++rep) {
                        auto t0 = Clock::now();
                        if (algoName == "bfs") r = firstHit(g, src, dst, t, mask);
                        else if (algoName == "bnb") r = branchAndBound(g, src, dst, t, mask, bnbLimit);
                        else r = aStar(g, src, dst, t, mask);
                        double us = chrono::duration<double, micro>(Clock::now() - t0).count();
                        bestUs = min(bestUs, us);
                    }
                    string name = maskName(mask);
                    fr << len << "," << pairId << "," << t.start << "," << t.end << "," << shortest << ","
                       << algoName << "," << name << "," << (r.found ? 1 : 0) << "," << r.firstLen << ","
                       << (r.found && r.firstLen == shortest ? 1 : 0) << "," << r.finalLen << ","
                       << (r.found && r.finalLen == shortest ? 1 : 0) << "," << r.expansions << ","
                       << bestUs << "," << (r.hitLimit ? 1 : 0) << "," << joinPath(g, r.firstPath) << "\n";
                    agg[{to_string(len), algoName, name}].add(r, shortest, bestUs);
                    agg[{"all", algoName, name}].add(r, shortest, bestUs);
                }
            }
        }
    }

    fs << "length,algorithm,strategy,runs,pct_found,pct_first_is_shortest,mean_extra_steps_first,"
          "pct_final_is_shortest,mean_extra_steps_final,mean_time_us,mean_expansions,limit_hits\n";
    fs.setf(ios::fixed);
    fs.precision(3);
    for (auto& [key, a] : agg) {
        auto& [len, algo, name] = key;
        double f = a.found ? (double)a.found : 1;
        fs << len << "," << algo << "," << name << "," << a.runs << ","
           << 100.0 * a.found / a.runs << "," << 100.0 * a.firstShortest / f << ","
           << a.extraFirst / f << "," << 100.0 * a.finalShortest / f << "," << a.extraFinal / f << ","
           << a.timeUs / a.runs << "," << a.expansions / a.runs << "," << a.limitHits << "\n";
    }
    cerr << "Wrote " << outRuns << " and " << outSummary << "\n";
    return 0;
}
