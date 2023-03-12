# Thread pool
Thread pool implementation

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


