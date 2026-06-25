cd /home/hypnotised/Desktop/projects/RMDB-os-practice/db2024/rmdb && \
zip -r /home/hypnotised/Desktop/projects/RMDB-os-practice/rmdb.zip * \
  -x '.DS_Store' \
  -x '.cache/*' \
  -x 'build/*' \
  -x '*.o' \
  -x '*.a' \
  -x '*.so'

# 
bash zip.sh
chmod +x zip.sh && ./zip.sh
