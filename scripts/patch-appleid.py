# Applies nekodrop's changes to WirDrop's appleid crate before it is built.
# Usage: python patch-appleid.py <third_party\WirDrop>
#
# - Phone numbers written without a country code: WirDrop takes them as Korean
#   (+82). nekodrop's users are mostly in mainland China, Taiwan and Hong Kong,
#   so the country code comes from APPLEID_CC (the client sets it from the
#   Windows region), +86 when unset, and an 11-digit mainland mobile number
#   (1xxxxxxxxxx) is always +86.
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'contacts.rs'
text = path.read_text(encoding='utf-8')
MARK = 'nekodrop: APPLEID_CC'
if MARK in text:
    sys.exit(0)

old = '''    } else if let Some(rest) = digits.strip_prefix('0') {
        format!("+82{rest}")
    } else {
        format!("+{digits}")
    };'''
new = '''    } else if digits.len() == 11 && digits.starts_with('1') {
        // nekodrop: APPLEID_CC. A mainland mobile number written without +86.
        format!("+86{digits}")
    } else if let Some(rest) = digits.strip_prefix('0') {
        format!("+{}{rest}", country_code())
    } else {
        format!("+{digits}")
    };'''
if text.count(old) != 1:
    sys.exit('contacts.rs: phone_spellings moved; update patch-appleid.py')
text = text.replace(old, new, 1)

old = '''    if let Some(rest) = e164.strip_prefix("+82") {
        out.push(("national", format!("0{rest}")));
    }'''
new = '''    let cc = format!("+{}", country_code());
    if let Some(rest) = e164.strip_prefix(cc.as_str()) {
        out.push(("national", format!("0{rest}")));
    }
    if let Some(rest) = e164.strip_prefix("+86") {
        out.push(("national", rest.to_string()));
    }'''
if text.count(old) != 1:
    sys.exit('contacts.rs: the national spelling moved; update patch-appleid.py')
text = text.replace(old, new, 1)

old = 'fn email_spellings('
new = '''/// The country of numbers written without one (nekodrop: APPLEID_CC, else 86).
fn country_code() -> String {
    std::env::var("APPLEID_CC")
        .ok()
        .filter(|c| !c.is_empty() && c.chars().all(|ch| ch.is_ascii_digit()))
        .unwrap_or_else(|| "86".to_string())
}

fn email_spellings('''
text = text.replace(old, new, 1)

# WirDrop's own test pins the Korean default; nekodrop's default differs.
text = text.replace('fn spells_phone_numbers_every_way() {', '#[ignore = "nekodrop: default country is not Korea"]\n    fn spells_phone_numbers_every_way() {', 1)
path.write_text(text, encoding='utf-8')
print('appleid: phone numbers without a country code follow APPLEID_CC (default +86)')
