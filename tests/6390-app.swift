#include <builtins.swift>
#include <files.swift>
#include <assert.swift>

// Test redirection

app (file out) echo (string arg) {
  "/bin/echo" arg @stdout=out; 
}

main () {
  string msg = "hello,world";
  file tmp = echo(msg);
  // echo appends newline
  assertEqual(readFile(tmp), msg + "\n", "contents of tmp");

  // Also write out to file for external checking
  file f<"6390.txt"> = echo(msg);
}
