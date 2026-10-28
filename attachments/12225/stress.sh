# usage: sp.sh <exe> <srcdir> <n> <conc> <load> <tag> [env...]
exe=$1; src=$2; n=$3; conc=$4; load=$5; tag=$6; shift 6
export LD_LIBRARY_PATH=$HOME/lcf-lvp/root/usr/lib SDL_AUDIODRIVER=dummy VK_ICD_FILENAMES=$HOME/lcf-lvp/icd/lvp.json ANYPS5_REQUIRE_VULKAN=1 GALLIUM_OVERRIDE_CPU_CAPS=avx APS5_SUBMIT_PROGRESS=1 APS5_NO_PIPELINE_JOURNAL=1
for e in "$@"; do export "$e"; done
cd $src
pids=()
for ((i=0;i<load;i++)); do (while :; do :; done) & pids+=($!); done
out=~/cf2-out/$tag; rm -rf $out; mkdir -p $out
worker() {
  local w=$1 i
  for ((i=w;i<=n;i+=conc)); do
    s=$(date +%s.%N)
    timeout 60 $PREFIX $exe > $out/$i.log 2>&1; rc=$?
    e=$(date +%s.%N)
    if [ $rc -ne 0 ]; then echo "run $i rc=$rc t=$(awk "BEGIN{print $e-$s}") $(grep -vE '^\[' $out/$i.log | head -2 | tr '\n' ' ' | cut -c1-200)"; else rm -f $out/$i.log; fi
  done
}
for ((w=1;w<=conc;w++)); do worker $w & done
wait $(jobs -p | grep -v -F -f <(printf '%s\n' "${pids[@]}")) 2>/dev/null
for p in "${pids[@]}"; do kill $p; done
wait 2>/dev/null
echo "$tag: $(ls $out | wc -l)/$n failed"
