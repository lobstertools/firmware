rm -r ./coverage_report
rm coverage.info
rm coverage_clean.info
pio run -e native -t clean
pio test -e native
lcov -d .pio/build/native/ -c -o coverage.info
lcov --remove coverage.info '*/test/*' '*/.pio/*' '/usr/*' '*/MockSessionHAL.h' -o coverage_clean.info
genhtml coverage_clean.info -o coverage_report --ignore-errors category 