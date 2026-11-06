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

#include <algorithm>
#include <cctype>
#include <vector>

#include "error.h"

namespace adbcpq {

namespace {

enum class TokenKind { kWord, kString, kComment, kPunct };

struct Token {
  TokenKind kind;
  size_t begin;
  size_t end;
  // For comments and dollar-quoted strings, the text between the delimiters.
  size_t body_begin = 0;
  size_t body_end = 0;
  bool dollar_quoted = false;
};

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// Return the length of the "$tag$" opening a dollar quote at sql[start], or 0. Mirrors
// the statement splitter's DollarQuoteTag so both agree on what is quoted.
size_t DollarQuoteTagLength(const std::string& sql, size_t start) {
  if (sql[start] != '$') return 0;
  if (start > 0 && IsIdentifierChar(sql[start - 1])) return 0;
  size_t i = start + 1;
  if (i < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i]))) return 0;
  while (i < sql.size() && IsIdentifierChar(sql[i])) i++;
  return i < sql.size() && sql[i] == '$' ? i - start + 1 : 0;
}

// Splits SQL into words, string literals (including quoted identifiers and
// dollar-quoted strings), comments and single-character punctuation.
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
      const size_t begin = i;
      while (i < n && sql[i] != '\n') i++;
      Token token{TokenKind::kComment, begin, i};
      token.body_begin = begin + 2;
      token.body_end = i;
      tokens.push_back(token);
    } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      const size_t begin = i;
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
      Token token{TokenKind::kComment, begin, i};
      token.body_begin = begin + 2;
      token.body_end = depth == 0 ? i - 2 : i;
      tokens.push_back(token);
    } else if (c == '\'' || c == '"') {
      const size_t begin = i++;
      // E'...' strings treat backslash as an escape.
      const bool escapes = c == '\'' && begin > 0 &&
                           (sql[begin - 1] == 'E' || sql[begin - 1] == 'e') &&
                           (begin < 2 || !is_word(sql[begin - 2]));
      while (i < n) {
        if (escapes && sql[i] == '\\') {
          i = std::min(i + 2, n);
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
      tokens.push_back({TokenKind::kString, begin, std::min(i, n)});
    } else if (const size_t tag_length = DollarQuoteTagLength(sql, i)) {
      const std::string tag = sql.substr(i, tag_length);
      const size_t close = sql.find(tag, i + tag_length);
      const size_t end = close == std::string::npos ? n : close + tag_length;
      Token token{TokenKind::kString, i, end};
      token.dollar_quoted = true;
      token.body_begin = i + tag_length;
      token.body_end = close == std::string::npos ? n : close;
      tokens.push_back(token);
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

// Whether a setting or parameter name has a secret, token, password or key part. Parts
// are separated by '_' or '.', so s3_access_key_id counts but monkey and tokenizer
// don't.
bool IsCredentialName(const std::string& name) {
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find_first_of("_.", start);
    if (end == std::string::npos) end = name.size();
    std::string part = name.substr(start, end - start);
    for (char& c : part)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (part == "SECRET" || part == "TOKEN" || part == "PASSWORD" || part == "PASSWD" ||
        part == "KEY") {
      return true;
    }
    start = end + 1;
  }
  return false;
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
  std::string name = sql.substr(tokens[j].begin, tokens[j].end - tokens[j].begin);
  if (tokens[j].kind == TokenKind::kString) {
    if (name.size() < 2 || name.front() != '"') return 0;
    name = name.substr(1, name.size() - 2);
  }
  if (!IsCredentialName(name)) return 0;
  j++;
  return IsPunct(sql, tokens, j, '=') || IsWord(sql, tokens, j, "TO") ? j : 0;
}

bool IsNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

// Mask name=value pairs with a credential name. The value runs to the next '&' or quote
// (or the end), so a value with spaces is masked whole.
std::string ScrubParameters(const std::string& text) {
  std::string out;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (!IsNameChar(text[i]) || (i > 0 && IsNameChar(text[i - 1]))) {
      out += text[i++];
      continue;
    }
    size_t end = i;
    while (end < n && IsNameChar(text[end])) end++;
    out.append(text, i, end - i);
    if (end < n && text[end] == '=' && IsCredentialName(text.substr(i, end - i))) {
      out += '=';
      size_t j = end + 1;
      while (j < n && text[j] != '&') {
        if (text[j] == '\\') {
          j = std::min(j + 2, n);
        } else if (text[j] == '\'' || text[j] == '"') {
          if (j + 1 < n && text[j + 1] == text[j]) {
            j += 2;  // doubled quote stays inside the literal
          } else {
            break;
          }
        } else {
          j++;
        }
      }
      out += "...";
      i = j;
    } else {
      i = end;
    }
  }
  return out;
}

// Mask the password in scheme://user:password@host.
std::string ScrubUserinfo(const std::string& text) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    const size_t scheme = text.find("://", i);
    if (scheme == std::string::npos) break;
    const size_t start = scheme + 3;
    size_t j = start;
    size_t colon = std::string::npos;
    while (j < text.size() &&
           std::string_view("/@'\" \t\r\n").find(text[j]) == std::string_view::npos) {
      if (text[j] == ':' && colon == std::string::npos) colon = j;
      j++;
    }
    if (j < text.size() && text[j] == '@' && colon != std::string::npos) {
      out.append(text, i, colon + 1 - i);
      out += "...";
      i = j;
    } else {
      out.append(text, i, start - i);
      i = start;
    }
  }
  out.append(text, i, std::string::npos);
  return out;
}

std::string ScrubText(const std::string& text) {
  return ScrubUserinfo(ScrubParameters(text));
}

constexpr int kMaxDollarDepth = 8;

std::string Redact(const std::string& text, int depth) {
  const std::vector<Token> all = Tokenize(text);
  std::vector<Token> significant;
  for (const Token& token : all) {
    if (token.kind != TokenKind::kComment) significant.push_back(token);
  }

  // Find where to cut the statement, and what replaces the rest.
  size_t cut = text.size();
  std::string suffix;
  for (size_t i = 0; i < significant.size() && suffix.empty(); i++) {
    if (const size_t after = MatchSecretStatement(text, significant, i)) {
      for (size_t j = after; j < significant.size(); j++) {
        if (IsPunct(text, significant, j, '(')) {
          cut = significant[j].begin;
          suffix = "(...)";
          break;
        }
      }
    } else if (const size_t op = MatchCredentialSetting(text, significant, i)) {
      cut = significant[op].end;
      suffix = " ...";
    }
  }

  // Everything before the cut keeps its text, except literals and comments, which can
  // hold credentials of their own.
  std::string out;
  size_t pos = 0;
  for (const Token& token : all) {
    if (token.begin >= cut) break;
    if (token.kind != TokenKind::kString && token.kind != TokenKind::kComment) continue;
    out.append(text, pos, token.begin - pos);
    if (token.dollar_quoted) {
      // The body is SQL (e.g. a macro), so redact it as such.
      out.append(text, token.begin, token.body_begin - token.begin);
      if (depth >= kMaxDollarDepth) {
        out += "...";
      } else {
        out += Redact(text.substr(token.body_begin, token.body_end - token.body_begin),
                      depth + 1);
      }
      out.append(text, token.body_end, token.end - token.body_end);
    } else if (token.kind == TokenKind::kComment) {
      out.append(text, token.begin, token.body_begin - token.begin);
      out += ScrubText(text.substr(token.body_begin, token.body_end - token.body_begin));
      out.append(text, token.body_end, token.end - token.body_end);
    } else {
      out += ScrubText(text.substr(token.begin, token.end - token.begin));
    }
    pos = token.end;
  }
  if (pos < cut) out.append(text, pos, cut - pos);
  return out + suffix;
}

}  // namespace

std::string RedactSecretStatements(std::string_view query) {
  return Redact(std::string(query), 0);
}

void InstallMotherDuckQueryRedactor() { SetQueryRedactor(&RedactSecretStatements); }

}  // namespace adbcpq
