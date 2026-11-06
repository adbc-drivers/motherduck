// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Masks credentials in SQL text that would otherwise appear in error messages.

#include "motherduck_redact.h"

#include <cctype>
#include <regex>
#include <vector>

#include "error.h"

namespace adbcpq {

namespace {

enum class TokenKind { kWord, kString, kPunct };

struct Token {
  TokenKind kind;
  size_t begin;
  size_t end;
};

// Splits SQL into words, string literals (including quoted identifiers and
// dollar-quoted strings) and single-character punctuation. Comments and
// whitespace are dropped, as the parser treats them as whitespace.
std::vector<Token> Tokenize(const std::string& sql) {
  std::vector<Token> tokens;
  const size_t n = sql.size();
  size_t i = 0;
  auto is_word_start = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
  };
  auto is_word = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
  };
  while (i < n) {
    const char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
    } else if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') i++;
    } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      int depth = 1;
      i += 2;
      while (i < n && depth > 0) {
        if (sql.compare(i, 2, "*/") == 0) {
          depth--;
          i += 2;
        } else if (sql.compare(i, 2, "/*") == 0) {
          depth++;
          i += 2;
        } else {
          i++;
        }
      }
    } else if (c == '\'' || c == '"') {
      const size_t begin = i++;
      // E'...' strings treat backslash as an escape.
      const bool escapes = c == '\'' && begin > 0 &&
                           (sql[begin - 1] == 'E' || sql[begin - 1] == 'e') &&
                           (begin < 2 || !is_word(sql[begin - 2]));
      while (i < n) {
        if (escapes && sql[i] == '\\') {
          i += 2;
          continue;
        }
        if (sql[i] == c) {
          if (i + 1 < n && sql[i + 1] == c) {
            i += 2;  // doubled quote escapes itself
            continue;
          }
          i++;
          break;
        }
        i++;
      }
      tokens.push_back({TokenKind::kString, begin, i});
    } else if (c == '$' && sql.find('$', i + 1) != std::string::npos &&
               (sql.find('$', i + 1) == i + 1 || is_word_start(sql[i + 1]))) {
      // $tag$ ... $tag$
      const size_t tag_end = sql.find('$', i + 1);
      const std::string tag = sql.substr(i, tag_end - i + 1);
      const size_t close = sql.find(tag, tag_end + 1);
      const size_t end = close == std::string::npos ? n : close + tag.size();
      tokens.push_back({TokenKind::kString, i, end});
      i = end;
    } else if (is_word_start(c)) {
      const size_t begin = i;
      while (i < n && is_word(sql[i])) i++;
      tokens.push_back({TokenKind::kWord, begin, i});
    } else {
      tokens.push_back({TokenKind::kPunct, i, i + 1});
      i++;
    }
  }
  return tokens;
}

std::string Upper(const std::string& sql, const Token& token) {
  std::string out = sql.substr(token.begin, token.end - token.begin);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool IsWord(const std::string& sql, const std::vector<Token>& tokens, size_t i,
            const char* word) {
  return i < tokens.size() && tokens[i].kind == TokenKind::kWord &&
         Upper(sql, tokens[i]) == word;
}

bool IsPunct(const std::string& sql, const std::vector<Token>& tokens, size_t i, char c) {
  return i < tokens.size() && tokens[i].kind == TokenKind::kPunct &&
         sql[tokens[i].begin] == c;
}

// CREATE|ALTER [OR REPLACE] [PERSISTENT|TEMPORARY|TEMP] SECRET ... (
// Returns the index of the token after SECRET, or 0 if there is no match.
size_t MatchSecretStatement(const std::string& sql, const std::vector<Token>& tokens,
                            size_t i) {
  if (!IsWord(sql, tokens, i, "CREATE") && !IsWord(sql, tokens, i, "ALTER")) return 0;
  size_t j = i + 1;
  if (IsWord(sql, tokens, j, "OR") && IsWord(sql, tokens, j + 1, "REPLACE")) j += 2;
  if (IsWord(sql, tokens, j, "PERSISTENT") || IsWord(sql, tokens, j, "TEMPORARY") ||
      IsWord(sql, tokens, j, "TEMP")) {
    j++;
  }
  return IsWord(sql, tokens, j, "SECRET") ? j + 1 : 0;
}

// SET [GLOBAL|LOCAL|SESSION|VARIABLE] <name> {=|TO} ... where the name looks like a
// credential setting, e.g. s3_secret_access_key. Returns the index of the operator.
size_t MatchCredentialSetting(const std::string& sql, const std::vector<Token>& tokens,
                              size_t i) {
  if (!IsWord(sql, tokens, i, "SET")) return 0;
  size_t j = i + 1;
  if (IsWord(sql, tokens, j, "GLOBAL") || IsWord(sql, tokens, j, "LOCAL") ||
      IsWord(sql, tokens, j, "SESSION") || IsWord(sql, tokens, j, "VARIABLE")) {
    j++;
  }
  if (j >= tokens.size() || tokens[j].kind == TokenKind::kPunct) return 0;
  std::string name = Upper(sql, tokens[j]);
  if (tokens[j].kind == TokenKind::kString) {
    if (name.size() < 2 || name.front() != '"') return 0;
    name = name.substr(1, name.size() - 2);
  }
  // Match whole underscore/dot-separated parts, so monkey and tokenizer don't count.
  bool credential = false;
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find_first_of("_.", start);
    if (end == std::string::npos) end = name.size();
    const std::string part = name.substr(start, end - start);
    if (part == "SECRET" || part == "TOKEN" || part == "PASSWORD" || part == "PASSWD" ||
        part == "KEY") {
      credential = true;
      break;
    }
    start = end + 1;
  }
  if (!credential) return 0;
  j++;
  return IsPunct(sql, tokens, j, '=') || IsWord(sql, tokens, j, "TO") ? j : 0;
}

}  // namespace

std::string RedactSecretStatements(std::string_view query) {
  const std::string text(query);
  const std::vector<Token> tokens = Tokenize(text);

  for (size_t i = 0; i < tokens.size(); i++) {
    if (const size_t after = MatchSecretStatement(text, tokens, i)) {
      for (size_t j = after; j < tokens.size(); j++) {
        if (IsPunct(text, tokens, j, '(')) {
          return text.substr(0, tokens[j].begin) + "(...)";
        }
      }
      continue;
    }
    if (const size_t op = MatchCredentialSetting(text, tokens, i)) {
      return text.substr(0, tokens[op].end) + " ...";
    }
  }

  // Connection strings such as ATTACH 'md:db?motherduck_token=...'.
  static const std::regex kCredentialParam(
      R"(\b((?:[A-Za-z0-9]+_)*(?:token|password|passwd|secret|key)=)(?:\\.|[^&\s'"\\])+)",
      std::regex::icase | std::regex::optimize);
  // user:password@host in URLs.
  static const std::regex kUserinfo(R"((://[^/:@\s'"]*:)[^/@\s'"]+@)",
                                    std::regex::optimize);
  std::string out;
  size_t copied = 0;
  for (const Token& token : tokens) {
    if (token.kind != TokenKind::kString) continue;
    const std::string literal = text.substr(token.begin, token.end - token.begin);
    const std::string redacted = std::regex_replace(
        std::regex_replace(literal, kUserinfo, "$1...@"), kCredentialParam, "$1...");
    if (redacted != literal) {
      out += text.substr(copied, token.begin - copied) + redacted;
      copied = token.end;
    }
  }
  if (copied == 0) return text;
  return out + text.substr(copied);
}

void InstallMotherDuckQueryRedactor() { SetQueryRedactor(&RedactSecretStatements); }

}  // namespace adbcpq
