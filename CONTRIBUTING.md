# Contributing to nevla

Thanks for helping! Bug reports, docs fixes and features are all welcome.

## Development setup

1. Install the MongoDB C++ driver (see [README](README.md#requirements-and-installation)).
2. Start a MongoDB **replica set** (transactions and change streams are part of the tests):

   ```bash
   docker run -d --name mongo -p 27017:27017 mongo:7 --replSet rs0
   docker exec mongo mongosh --quiet --eval 'rs.initiate()'
   ```

3. Build and test:

   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j
   ./build/nevla_tests                      # must print "... passed, 0 failed"
   ```

   Another server: `NEVLA_TEST_URI="mongodb://host:27017/?replicaSet=rs0" ./build/nevla_tests`.

4. Before a pull request, also run with sanitizers:

   ```bash
   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
   cmake --build build-asan -j && ./build-asan/nevla_tests
   ```

## Guidelines

- C++20, no extra dependencies besides `mongocxx`.
- Keep the public API Mongoose-like: if Mongoose has the feature, use the same name and semantics
  (C++ keywords get a trailing underscore: `virtual_`, `static_`, `or_`).
- Every change needs a test in `tests/test.cc` and, for user-visible features, a README section.
- Errors: throw a `nevla::Error` subclass with an English message.
- Public headers must compile warning-free with `-Wall -Wextra`.
- Format with the repository's `.clang-format` (`clang-format -i src/*.cc include/nevla/*.hpp`).
- Add an entry to `CHANGELOG.md` under *Unreleased*.

## Reporting bugs

Please include: nevla version, compiler, mongocxx and MongoDB versions, a minimal schema + code that
reproduces the problem, and the full error message.
