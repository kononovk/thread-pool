# Thread pool
Very simple thread pool implementation for studying purposes

```C++
utils::thread_pool tp(1);
bool runned = false;
auto future = tp.submit([&runned]() -> bool {
  runned = true;
  return true;
});
assert(future.get() == true);
assert(runned == true);
```


