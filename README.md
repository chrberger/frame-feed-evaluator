# frame-feed-evaluator
Repository to systematically evaluate video compression codecs

Feed:
```
./frame-feed-evaluator  --folder=../pngs/ --name=i420 --delay=0 --cid=111
```

Client:
```
docker run --rm -ti --init --net=host --ipc=host -v /tmp:/tmp x264:latest --cid=111 --width=640 --height=480 --name=i420 --verbose
```
