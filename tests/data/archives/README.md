Archive regression fixtures:

- `sample.tar.bz2` contains the same tar stream as `sample.tar.gz`, compressed with bzip2.
- `encrypted-headers.7z` and `encrypted-content.7z` contain `message.txt` with the bytes
  `archive payload\n`. Both use password `archive test`; only the former encrypts headers.
- `encrypted-unicode.7z` has the same payload, encrypted headers, and password
  `päss 🔒 archive test` to exercise UTF-8 password transport.

The new 7z fixtures were generated using p7zip 17.05:

```sh
printf 'archive payload\n' > message.txt
7z a -t7z -p'archive test' -mhe=on encrypted-headers.7z message.txt
7z a -t7z -p'archive test' -mhe=off encrypted-content.7z message.txt
7z a -t7z -p'päss 🔒 archive test' -mhe=on encrypted-unicode.7z message.txt
```

These are public test passwords, not credentials. The older `encrypted.7z` fixture is
retained for the test of extraction without a password callback.

`encrypted-zipcrypto.zip` and `encrypted-aes.zip` contain the same `message.txt`
payload and use password `archive test`. They were generated with 7-Zip 26.04:

```sh
7zz a -tzip -mem=ZipCrypto -p'archive test' encrypted-zipcrypto.zip message.txt
7zz a -tzip -mem=AES256 -p'archive test' encrypted-aes.zip message.txt
```

The four encrypted RAR fixtures come from libarchive commit
`4a5944b726d24c8e86589ad09cef958c6f1a6e9c`, decoded from uuencode without changing
the archive bytes. See [libarchive-COPYING](libarchive-COPYING) for the license.
Their password is `password`; they contain `a.txt` through `d.txt`, each containing
`This is from <filename>` without a trailing newline.

| Local fixture | Upstream file in `libarchive/test/` |
| --- | --- |
| `encrypted-content.rar` | `test_read_format_rar4_solid_encrypted.rar.uu` |
| `encrypted-headers.rar` | `test_read_format_rar4_encrypted_filenames.rar.uu` |
| `encrypted-content-rar5.rar` | `test_read_format_rar5_solid_encrypted.rar.uu` |
| `encrypted-headers-rar5.rar` | `test_read_format_rar5_encrypted_filenames.rar.uu` |

Source: https://github.com/libarchive/libarchive/tree/4a5944b726d24c8e86589ad09cef958c6f1a6e9c/libarchive/test
