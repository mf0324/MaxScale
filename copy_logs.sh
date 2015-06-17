#!/bin/bash

#set -x

if [ -z $1 ]; then
	echo "Test name missing"
	logs_dir="LOGS/nomane"
else

	logs_dir="LOGS/$1"
	rm -rf $logs_dir
fi

echo "Creating log dir in workspace $logs_dir"
mkdir -p $logs_dir
if [ $? -ne 0 ]; then
        echo "Error creating log dir"
fi


echo "log_dir:         $logs_dir"
echo "maxscale_sshkey: $maxscale_sshkey"
echo "maxscale_IP:     $maxscale_IP"
scp -i $maxscale_sshkey -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no $access_user@$maxscale_IP:$maxscale_log_dir/* $logs_dir
if [ $? -ne 0 ]; then
	echo "Error copying Maxscale logs"
fi
scp -i $maxscale_sshkey -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no $access_user@$maxscale_IP:/tmp/core* $logs_dir
#scp -i $maxscale_sshkey -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no $access_user@$maxscale_IP:/root/core* $logs_dir
scp -i $maxscale_sshkey -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no $access_user@$maxscale_IP:$maxscale_cnf $logs_dir
chmod a+r $logs_dir/*

