curl -X POST 'http://192.168.4.1/adduser/bulk' \
  --data-urlencode "a=YOUR_ADMIN_KEY" \
  --data-urlencode "csv@/path/to/villas_108_import.csvi"


curl -sS -X POST 'http://192.168.4.1/adduser/bulk' \
  --data-urlencode 'a=YOUR_ADMIN_TOKEN' \
  --data-urlencode 'csv=9000000001,MySecret10,1
9000000002,OtherSec10,2'

curl -sS -X POST 'http://192.168.4.1/adduser/bulk' \
  --data-urlencode 'a=YOUR_ADMIN_TOKEN' \
  --data-urlencode 'csv@/home/ravikumar/gate-controller/villas_108_import.csv'

curl -sS 'http://192.168.4.1/adduser?a=YOUR_ADMIN_TOKEN' | jq .
