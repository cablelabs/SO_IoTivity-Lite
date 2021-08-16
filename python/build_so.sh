#!/bin/bash
echo `pwd`
cd ../port/linux
#make clean
#make clean
make OC_SO=1 SO_DPP=1 CLOUD=1 CLIENT=1 PKI=1 SECURE=1 libiotivity-lite-client-python.so
cd  ../../python
#mkdir pki_certs
cp -r ../apps/pki_certs/. ./pki_certs
#create certs
cd obt_web
#openssl req -new -newkey rsa:4096 -days 365 -nodes -x509 \
#    -subj "/C=US/ST=RFOTM/L=Springfield/O=Dis/CN=www.example.com" \
#    -keyout key.pem  -out cert.pem

#create virtual environement
#echo "Create virtual environment"
#python3 -m venv ocf
#cd ocf
#_envdir="`pwd`"
#source bin/activate
#pip install -r requirements.txt
