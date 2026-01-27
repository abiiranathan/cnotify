package main

import "fmt"

func main() {
	fmt.Printf("Hello World\n")
	for i := range 10 {
		fmt.Printf("i=%d\n", i)
	}
}
