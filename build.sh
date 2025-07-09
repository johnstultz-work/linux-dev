#!/bin/bash
export PATH=~/projects/llvm-project/build/bin/:$PATH

parallel  --help | grep "GNU" > /dev/null
if [ $? -ne 0 ]; then
	echo "Need GNU version of parallel."
	echo "Try 'apt-get install parallel'?"
	exit -1;
fi


do_build() {
	set +e
	cfg=$1
	if [[ $cfg == *"llvm"* ]]; then
		export LLVM=1
	fi
	echo "=====[Using $cfg...]====================="
	mkdir -p out/$cfg > /dev/null
	make O=out/$cfg  $cfg > /dev/null;
	make O=out/$cfg -j20 bzImage > /dev/null ;
	if [ $? -ne 0 ]; then
		echo "=====[ERROR: Config $cfg didn't build!]==="
		return -1;
	else
		echo "=====[$cfg build succeeded!]=============="
	fi
}

export -f do_build

set -e

CFGS="
  proxy_test_defconfig
  noproxy_test_defconfig
  proxy_nosmp_test_defconfig
  proxy_preempt_test_defconfig
  noproxy_schedcore_test_defconfig"
#  proxy_llvm_defconfig"

date;
git log --pretty="Building %C(yellow)%h%C(reset) %s" HEAD~1..HEAD
time parallel --halt-on-error 2 -j 0 --group do_build {} ::: $CFGS

