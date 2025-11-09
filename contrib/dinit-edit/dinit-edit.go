package main

import (
	"fmt"
	"os"
	"path"
	"slices"
	"syscall"
)

func getServiceDirs() (serviceDirs []string) {
	if os.Getuid() == 0 {
		serviceDirs = []string{"/etc/dinit.d", "/run/dinit.d", "/usr/local/lib/dinit.d", "/lib/dinit.d"}
	} else {
		if xdg_home, ok := os.LookupEnv("XDG_CONFIG_HOME"); ok {
			serviceDirs = append(serviceDirs, path.Join(xdg_home, "dinit.d"))
		}
		if home, ok := os.LookupEnv("HOME"); ok {
			serviceDirs = append(serviceDirs, path.Join(home, ".config/dinit.d"))
		}
		serviceDirs = slices.Concat(serviceDirs, []string{"/etc/dinit.d/user", "/usr/lib/dinit.d/user", "/usr/local/lib/dinit.d/user"})
	}
	return
}

type Service struct {
	Name string
	Path string
}

func main() {
	dirs := getServiceDirs()
	services := []Service{}
	for _, dir := range dirs {
		entries, err := os.ReadDir(dir)
		if err != nil {
			// missing dir is ok
			if !os.IsNotExist(err) {
				fmt.Println(err)
			}
		} else {
			for _, entry := range entries {
				if entry.Type().IsRegular() {
					name := entry.Name()
					path := path.Join(dir, name)
					services = append(services, Service{
						Name: name,
						Path: path,
					})
				}
			}
		}
	}

	if len(os.Args) < 2 {
		listAll(services)
	} else {
		edit(services, os.Args[1])
	}
}

func listAll(services []Service) {
	longest := 0
	for _, srv := range services {
		longest = max(longest, len(srv.Name))
	}
	for _, srv := range services {
		fmt.Fprintf(os.Stdout, "%-*s%s\n", longest+4, srv.Name, srv.Path)
	}
}

func edit(services []Service, name string) {
	i := slices.IndexFunc(services, func(srv Service) bool { return srv.Name == name })
	if i < 0 {
		fmt.Println("service not found:", name)
		os.Exit(1)
	} else {
		if editor_sh, ok := os.LookupEnv("EDITOR"); ok {
			err := syscall.Exec("/bin/sh", []string{"sh", "-c", editor_sh + " " + services[i].Path}, os.Environ())
			if err != nil {
				fmt.Println(err)
				os.Exit(1)
			}
		}
	}
}
