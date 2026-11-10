#!/usr/bin/env python3
"""The identity this boot presents, as a header the loader compiles in.

usage: mkidentityconf.py [devices/<device>/identity.conf] > xnuloader/identity_conf.h

Two kinds of value, and they come from different places:

  - the serial number and the UDID's inputs (serial-number, mlb-serial-number, model-number, region-info,
    regulatory-model-number, unique-chip-id, chip-id, board-id, wifi-mac, bluetooth-mac): what iBoot
    would have taken from the factory configuration of a real phone;
  - the model (model, board, product-name): what Darwin user space is to think it runs on. Left out, the
    one in identity.h is kept -- the iPhone whose files these are (mkidentity.sh, from the IPSW).

A missing file is not an error: the defaults below are written, so a device without one still builds.
An unknown key is an error, so that a typo does not silently leave a value at its default.

Keys:
  serial-number, mlb-serial-number, model-number, region-info, regulatory-model-number
      ASCII, at most 31 characters; the device tree pads the field to 32 bytes with zeros
  unique-chip-id, chip-id, board-id
      integers, decimal or 0x hex; unique-chip-id is 64-bit, the other two 32-bit
  wifi-mac, bluetooth-mac
      six bytes, "02:52:44:57:00:01"
  model, board, product-name (optional)
      model=iPhone13,1 board=D83AP product-name="iPhone 12 mini": the root node's model, target-type,
      target-sub-type and compatible, and /product's product-name. board is the board name iBoot
      reports, ending AP (target-type is that without AP); compatible becomes
      "<board>\\0<model>\\0AppleARM\\0".
"""
import re
import sys

DEFAULTS = {
    'serial-number': 'RDW00POCOX5P',
    'mlb-serial-number': 'RDW00POCOX5PMLB00',
    'model-number': 'MQAC2',
    'region-info': 'LL/A',
    'regulatory-model-number': 'A1865',
    'unique-chip-id': '0x000052445730001',
    'chip-id': '0x8015',
    'board-id': '0x06',
    'wifi-mac': '02:52:44:57:00:01',
    'bluetooth-mac': '02:52:44:57:00:02',
}
OPTIONAL = ('model', 'board', 'product-name')
STRINGS = {
    'serial-number': 'HK_SERIAL', 'mlb-serial-number': 'HK_MLB', 'model-number': 'HK_MODEL_NUMBER',
    'region-info': 'HK_REGION', 'regulatory-model-number': 'HK_REGULATORY',
}
NUMBERS = {'unique-chip-id': (64, 'HK_UNIQUE_CHIP_ID'), 'chip-id': (32, 'HK_CHIP_ID'),
           'board-id': (32, 'HK_BOARD_ID')}
MACS = {'wifi-mac': 'HK_MAC_WIFI', 'bluetooth-mac': 'HK_MAC_BLUETOOTH'}
KNOWN = tuple(DEFAULTS) + OPTIONAL


def read_config(path):
    values = {}
    for number, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if '=' not in line:
            sys.exit(f'mkidentityconf.py: {path}:{number}: not a key=value line')
        key, _, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if key not in KNOWN:
            sys.exit(f'mkidentityconf.py: {path}:{number}: unknown key {key!r}')
        if key in values:
            sys.exit(f'mkidentityconf.py: {path}:{number}: {key} twice')
        values[key] = value
    missing = sorted(set(DEFAULTS) - set(values))
    if missing:
        sys.exit(f'mkidentityconf.py: {path}: missing {", ".join(missing)}')
    return values


def string_value(path, key, text, limit=31):
    if not text.isascii() or not text:
        sys.exit(f'mkidentityconf.py: {path}: {key} has to be non-empty ASCII')
    if len(text) > limit:
        sys.exit(f'mkidentityconf.py: {path}: {key} is {len(text)} characters, at most {limit} fit')
    if '"' in text or '\\' in text:
        sys.exit(f'mkidentityconf.py: {path}: {key} cannot hold a quote or a backslash')
    return text


def number_value(path, key, text, bits):
    try:
        value = int(text, 0)
    except ValueError:
        sys.exit(f'mkidentityconf.py: {path}: {key} is not a number: {text!r}')
    if value <= 0 or value >= 1 << bits:
        sys.exit(f'mkidentityconf.py: {path}: {key} does not fit {bits} bits')
    return value


def mac_value(path, key, text):
    parts = text.split(':')
    if len(parts) != 6 or any(not re.fullmatch(r'[0-9a-fA-F]{2}', p) for p in parts):
        sys.exit(f'mkidentityconf.py: {path}: {key} is not six hex bytes: {text!r}')
    return ', '.join('0x%s' % p.lower() for p in parts)


def model_values(path, values):
    """The overrides of the model, as (root list, product list); empty when the file has none."""
    if not any(k in values for k in OPTIONAL):
        return [], []
    missing = [k for k in OPTIONAL if k not in values]
    if missing:
        sys.exit(f'mkidentityconf.py: {path}: {", ".join(missing)} missing; '
                 'model, board and product-name go together')
    model = values['model']
    board = values['board']
    if not re.fullmatch(r'iPhone\d+,\d+', model):
        sys.exit(f'mkidentityconf.py: {path}: model is not of the form iPhone13,1: {model!r}')
    if not re.fullmatch(r'D[A-Za-z0-9]{1,4}AP', board):
        sys.exit(f'mkidentityconf.py: {path}: board is not of the form D52gAP: {board!r}')
    product = string_value(path, 'product-name', values['product-name'], 31)
    target = board[:-2]                     # D83AP -> D83
    # "<board>\0<model>\0AppleARM\0", the bytes the phone's own tree carries as compatible
    compatible = (board + '\0' + model + '\0' + 'AppleARM')
    root = [
        ('target-type', target),
        ('target-sub-type', board),
        ('compatible', compatible),
        ('model', model),
    ]
    # product-name and product-description are both what the phone is called: Darwin's own tree
    # carries the marketing name in both, and the interface reads one or the other
    return root, [('product-name', product), ('product-description', product)]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if path is not None:
        try:
            values = read_config(path)
        except OSError as e:
            sys.exit(f'mkidentityconf.py: {e}')
        source = path
    else:
        values = dict(DEFAULTS)
        source = 'the defaults in mkidentityconf.py'

    root_override, product_override = model_values(path or source, values)
    out = sys.stdout

    def table(name, entries):
        out.write('static const struct identity_override %s[] = {\n' % name)
        if not entries:
            out.write('\t{ "", "", 0 },\t/* none: identity.h keeps its own */\n')
        for key, value in entries:
            out.write('\t{ "%s", "%s", %d },\n' % (key, value.replace('\0', '\\0'), len(value) + 1))
        out.write('};\n')

    out.write('/* Generated by mkidentityconf.py from %s. Do not edit. */\n\n' % source)
    out.write('/* A model property the boot presents in place of the one in identity.h. */\n')
    out.write('struct identity_override { const char *name; const char *value; uint32_t length; };\n\n')
    for key, name in STRINGS.items():
        out.write('#define %-16s "%s"\n' % (name, string_value(source, key, values[key])))
    for key, (bits, name) in NUMBERS.items():
        suffix = 'ULL' if bits == 64 else 'u'
        out.write('#define %-16s 0x%0*x%s\n' % (name, bits // 4, number_value(source, key, values[key], bits), suffix))
    for key, name in MACS.items():
        out.write('#define %-16s { %s }\n' % (name, mac_value(source, key, values[key])))
    out.write('\n/* What the phone is taken for: the model, unless identity.h is to be believed. */\n')
    out.write('#define HK_ROOT_OVERRIDE_COUNT    %d\n' % len(root_override))
    table('hk_root_override', root_override)
    out.write('#define HK_PRODUCT_OVERRIDE_COUNT %d\n' % len(product_override))
    table('hk_product_override', product_override)


if __name__ == '__main__':
    main()
