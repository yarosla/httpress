# httpress

High performance HTTP server stress & benchmark utility.

Inspired by [weighttp](http://redmine.lighttpd.net/projects/weighttp/wiki) tool.

Main features:

* event driven (low memory footprint, large number of connections)
* multi-threaded (uses all cores of your CPU)
* SSL support (via GNUTLS library) with cipher suite selection

Compared to weighttp, httpress offers the following improvements:

* evenly distributes load between threads; does not allow one thread to finish much earlier than others
* promptly timeouts stucked connections, forces all hanging connections to close after the main run, does not allow hanging or interrupted connections to affect the measurement
* SSL support

## Usage

    httpress <options> <url>
    -n num   number of requests     (default: 1)
    -t num   number of threads      (default: 1)
    -c num   concurrent connections (default: 1)
    -k       keep alive             (default: no)
    -z pri   GNUTLS cipher priority (default: NORMAL)
    -h       show this help

    example: httpress -n 10000 -c 100 -t 4 -k http://localhost:8080/index.html

## Dependencies

* Depends on [[http://software.schmorp.de/pkg/libev.html|libev]] library
* Depends on [[http://www.gnu.org/software/gnutls/|GnuTLS]] library (if compiled with SSL support)

## Building from source

1. Prerequisite: [libev 4 library](http://software.schmorp.de/pkg/libev.html) (your distro might have it in repo; otherwise build from source)
1. SSL prerequisite: [GnuTLS 3.0 library](http://www.gnu.org/software/gnutls/) (better build from source)
1. Download [httpress source](https://github.com/yarosla/httpress)
1. Run `make` or `make -f Makefile.nossl`
1. Collect executable from bin subdirectory

## Author

httpress is written by Yaroslav Stavnichiy.

Home page: https://github.com/yarosla/httpress
