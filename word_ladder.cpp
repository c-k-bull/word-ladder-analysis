// word_ladder.cpp
// Randomly pairs words of the same length from a CSV and finds shortest word ladders.
//
// Build:  g++ -O2 -std=c++17 word_ladder.cpp -o word_ladder
// Usage:  ./word_ladder common_nouns_3_to_5.csv [options]
//
// Options:
//   --dict FILE        Word list allowed for the in-between steps of a ladder
//                      (CSV with words in the first column, or plain text, one word per line).
//                      Default: only words from the input CSV.
//   --all-pairs        Instead of random pairing, solve EVERY pair of same-length words
//                      (each unordered pair once). --seed is ignored.
//   --seed N           Random seed for pairing (default: random). Reuse a seed to get the same pairs.
//   --max-paths N      Cap on shortest paths listed per pair in the "all" file (default 1000).
//   --out-all FILE     Default: ladders_all_shortest.csv
//   --out-first FILE   Default: ladders_first_shortest.csv
//
// Output (one row per pair in the "first" file, one row per path in the "all" file):
//   pair_id, start, end, word_length, steps, [num_paths, truncated, path_index,] path
//   steps = number of letter changes; -1 and an empty path mean no ladder exists.
//   path looks like: cold>cord>card>ward>warm

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

static string clean(string s) {
    // trim whitespace, CR, and surrounding quotes; lowercase
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

// Reads the first column of a CSV or a plain word list. Skips a header row
// (a first line that contains a comma and starts with "word").
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

// All the words of one length, as a graph where neighbors differ by one letter.
struct Graph {
    vector<string> words;
    unordered_map<string, int> index;
    vector<vector<int>> adj;

    int add(const string& w) {
        auto it = index.find(w);
        if (it != index.end()) return it->second;
        index[w] = (int)words.size();
        words.push_back(w);
        return (int)words.size() - 1;
    }

    void buildEdges() {
        adj.assign(words.size(), {});
        for (int i = 0; i < (int)words.size(); ++i) {
            string w = words[i];
            for (size_t p = 0; p < w.size(); ++p) {
                char orig = w[p];
                for (char c = 'a'; c <= 'z'; ++c) {   // fixed order => deterministic "first found"
                    if (c == orig) continue;
                    w[p] = c;
                    auto it = index.find(w);
                    if (it != index.end()) adj[i].push_back(it->second);
                }
                w[p] = orig;
            }
        }
    }
};

// BFS from src recording EVERY parent on a shortest route (parents kept in discovery order).
// Returns distance to dst, or -1. With dst = -1 it searches the whole graph
// (used by --all-pairs, where one search from a word serves every target).
static int bfsAllParents(const Graph& g, int src, int dst, vector<vector<int>>& parents,
                         vector<int>& dist) {
    int n = (int)g.words.size();
    dist.assign(n, -1);
    parents.assign(n, {});
    queue<int> q;
    dist[src] = 0;
    q.push(src);
    while (!q.empty()) {
        int u = q.front(); q.pop();
        if (dst >= 0 && dist[dst] != -1 && dist[u] == dist[dst]) break;   // nothing shorter can follow
        for (int v : g.adj[u]) {
            if (dist[v] == -1) {
                dist[v] = dist[u] + 1;
                parents[v].push_back(u);
                q.push(v);
            } else if (dist[v] == dist[u] + 1) {
                parents[v].push_back(u);
            }
        }
    }
    return dst >= 0 ? dist[dst] : 0;
}

// Counts shortest paths (capped to avoid overflow) so we can report truncation honestly.
static uint64_t countPaths(int node, int src, const vector<vector<int>>& parents,
                           unordered_map<int, uint64_t>& memo) {
    if (node == src) return 1;
    auto it = memo.find(node);
    if (it != memo.end()) return it->second;
    uint64_t total = 0;
    for (int p : parents[node]) {
        total += countPaths(p, src, parents, memo);
        if (total > (uint64_t)1e15) { total = (uint64_t)1e15; break; }
    }
    return memo[node] = total;
}

// Walks parents backward from dst to src, collecting up to maxPaths paths.
// The first path produced follows each node's first-discovered parent,
// so it matches the "first shortest found" path.
static void collectPaths(int node, int src, const vector<vector<int>>& parents,
                         vector<int>& stack, vector<vector<int>>& out, size_t maxPaths) {
    if (out.size() >= maxPaths) return;
    stack.push_back(node);
    if (node == src) {
        out.emplace_back(stack.rbegin(), stack.rend());
    } else {
        for (int p : parents[node]) {
            collectPaths(p, src, parents, stack, out, maxPaths);
            if (out.size() >= maxPaths) break;
        }
    }
    stack.pop_back();
}

static string joinPath(const Graph& g, const vector<int>& path) {
    string s;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) s += '>';
        s += g.words[path[i]];
    }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " input.csv [--dict FILE] [--seed N] [--max-paths N]"
             << " [--out-all FILE] [--out-first FILE]\n";
        return 1;
    }
    string inputPath = argv[1], dictPath, outAll = "ladders_all_shortest.csv",
           outFirst = "ladders_first_shortest.csv";
    uint64_t seed = random_device{}();
    size_t maxPaths = 1000;
    bool allPairs = false;
    for (int i = 2; i < argc; ++i) {
        string a = argv[i];
        auto next = [&]() -> string {
            if (i + 1 >= argc) { cerr << "Missing value for " << a << "\n"; exit(1); }
            return argv[++i];
        };
        if (a == "--dict") dictPath = next();
        else if (a == "--all-pairs") allPairs = true;
        else if (a == "--seed") seed = stoull(next());
        else if (a == "--max-paths") maxPaths = stoull(next());
        else if (a == "--out-all") outAll = next();
        else if (a == "--out-first") outFirst = next();
        else { cerr << "Unknown option " << a << "\n"; return 1; }
    }

    // Words to pair, grouped by length.
    map<size_t, vector<string>> byLen;
    {
        vector<string> input = readWords(inputPath);
        unordered_map<string, bool> seen;
        for (auto& w : input) if (!seen[w]) { seen[w] = true; byLen[w.size()].push_back(w); }
    }

    // One graph per length: input words plus (optionally) the dictionary words.
    map<size_t, Graph> graphs;
    for (auto& [len, ws] : byLen) for (auto& w : ws) graphs[len].add(w);
    if (!dictPath.empty())
        for (auto& w : readWords(dictPath))
            if (graphs.count(w.size())) graphs[w.size()].add(w);
    for (auto& [len, g] : graphs) g.buildEdges();

    ofstream fa(outAll), ff(outFirst);
    if (!fa || !ff) { cerr << "Cannot open output files\n"; return 1; }
    fa << "pair_id,start,end,word_length,steps,num_paths,truncated,path_index,path\n";
    ff << "pair_id,start,end,word_length,steps,path\n";

    long long pairId = 0, found = 0, missing = 0;

    // Writes one pair to both files, given a search that started at `a`.
    auto emit = [&](Graph& g, const string& a, const string& b, int src, int dst, int d,
                    const vector<vector<int>>& parents, unordered_map<int, uint64_t>& memo) {
        ++pairId;
        string base = to_string(pairId) + "," + a + "," + b + "," + to_string(g.words[src].size()) + ",";
        if (d < 0) {
            ++missing;
            ff << base << "-1,\n";
            fa << base << "-1,0,false,0,\n";
            return;
        }
        ++found;
        uint64_t total = countPaths(dst, src, parents, memo);
        vector<vector<int>> paths;
        vector<int> stack;
        collectPaths(dst, src, parents, stack, paths, maxPaths);
        bool truncated = total > paths.size();
        ff << base << d << "," << joinPath(g, paths[0]) << "\n";
        for (size_t k = 0; k < paths.size(); ++k)
            fa << base << d << "," << total << "," << (truncated ? "true" : "false") << ","
               << k + 1 << "," << joinPath(g, paths[k]) << "\n";
    };

    mt19937_64 rng(seed);
    for (auto& [len, ws] : byLen) {
        Graph& g = graphs[len];
        vector<vector<int>> parents;
        vector<int> dist;

        if (allPairs) {
            vector<string> sorted = ws;
            sort(sorted.begin(), sorted.end());
            for (size_t i = 0; i < sorted.size(); ++i) {
                int src = g.index.at(sorted[i]);
                bfsAllParents(g, src, -1, parents, dist);
                unordered_map<int, uint64_t> memo;   // path counts back to this source
                for (size_t j = i + 1; j < sorted.size(); ++j) {
                    int dst = g.index.at(sorted[j]);
                    emit(g, sorted[i], sorted[j], src, dst, dist[dst], parents, memo);
                }
            }
            continue;
        }

        vector<string> shuffled = ws;
        shuffle(shuffled.begin(), shuffled.end(), rng);
        if (shuffled.size() % 2)
            cerr << "Warning: odd number of " << len << "-letter words; '"
                 << shuffled.back() << "' is left unpaired\n";
        for (size_t i = 0; i + 1 < shuffled.size(); i += 2) {
            const string &a = shuffled[i], &b = shuffled[i + 1];
            int src = g.index.at(a), dst = g.index.at(b);
            int d = bfsAllParents(g, src, dst, parents, dist);
            unordered_map<int, uint64_t> memo;
            emit(g, a, b, src, dst, d, parents, memo);
        }
    }

    if (!allPairs) cerr << "Seed: " << seed << "\n";
    cerr
         << "Pairs: " << pairId << " (" << found << " connected, " << missing << " with no ladder)\n"
         << "Wrote " << outFirst << " and " << outAll << "\n";
    return 0;
}
