import json
import os
import sys
from deep_translator import GoogleTranslator

JSON_PATH = os.path.join("server", "src", "i18n", "diagnostics_es.json")

def main():
    if not os.path.exists(JSON_PATH):
        print(f"Error: {JSON_PATH} not found")
        sys.exit(1)
        
    with open(JSON_PATH, 'r', encoding='utf-8') as f:
        translations = json.load(f)
        
    translator = GoogleTranslator(source='en', target='es')
    
    count = 0
    for key, value in translations.items():
        if key == value:
            # Check if it has %s or %d
            # deep-translator might mess up %s. Let's protect them.
            safe_text = key.replace('%s', 'VAR_S').replace('%d', 'VAR_D').replace('%u', 'VAR_U').replace('%i', 'VAR_I')
            
            try:
                translated = translator.translate(safe_text)
                
                # Restore variables
                translated = translated.replace('VAR_S', '%s').replace('VAR_D', '%d').replace('VAR_U', '%u').replace('VAR_I', '%i')
                
                translations[key] = translated
                count += 1
                if count % 10 == 0:
                    print(f"Translated {count} strings...")
            except Exception as e:
                print(f"Error translating '{key}': {e}")
                
    with open(JSON_PATH, 'w', encoding='utf-8') as f:
        json.dump(translations, f, indent=4, ensure_ascii=False)
        
    print(f"Finished translating {count} strings.")

if __name__ == "__main__":
    main()
