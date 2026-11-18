#pragma once
/* Formula strings: write MongoDB expressions and filters like normal math / C++ conditions.

     expr("price * qty * (1 - discount)")
       -> {$multiply: ["$price", "$qty", {$subtract: [1, "$discount"]}]}

     formulaFilter("temp > 30 && room == 'lab'")
       -> {temp: {$gt: 30}, room: "lab"}             (a normal filter: uses indexes and schema casting)

     formulaFilter("used > limit")                   (field compared with field)
       -> {$expr: {$gt: ["$used", "$limit"]}}

   Language
     values     12  3.5  1e3  'text'  "text"  true  false  null  [1, 2, 3]  date('2026-11-17')  now()
     fields     price  address.city  items.0.qty  ($price works too)
     math       + - * / %   (unary -)       strings: concat(a, b, ...)
     compare    == != < <= > >=   (= also means ==)      x in [..]   x not in [..]
     logic      && || !    (and / or / not)
     functions  abs ceil floor round(x[, places]) trunc sqrt pow(x, y) exp ln log10 log(x, base)
                min(a, b, ...) max(...) sum(...) avg(...) size(array)
                concat lower upper trim len substr(s, start, count)
                if(cond, a, b) ifNull(a, b) exists(field)
                year month day hour minute second dayOfWeek (date[, 'Asia/Kolkata'])
                dateDiff(start, end, 'day') toInt toDouble toString toDate toBool
   Errors throw FormulaError with the column and a caret under the problem.                           */

#include <string>
#include <string_view>

#include "value.hpp"

namespace nevla {

class FormulaError : public Error {
  public:
    size_t column;  // 1-based
    FormulaError(const std::string &msg, size_t col) : Error(msg), column(col) {}
};

// Formula -> aggregation expression (for addFields / project / group / $expr)
Value expr(std::string_view formula);

// Formula -> query filter. Simple "field op constant" parts become normal filter conditions
// (index-friendly, cast by the schema); everything else becomes {$expr: ...}.
Value formulaFilter(std::string_view formula);

namespace detail {
// "price" / "address.city" / "$price": a plain field path, not a formula
bool isFieldPath(std::string_view s);
}  // namespace detail

}  // namespace nevla
