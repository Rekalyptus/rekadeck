"""Sample deck for the simulator: basic cards, multiple choice cards and a few image cards, from one-liners to long
vignettes, lists and tables, so the layouts get exercised.

kind "basic":  front, back, optional image (a function returning a 1-bit PIL image). In back, a newline starts a new
               line (for lists) and a blank line starts a new paragraph, with half a line of space
kind "choice": front, options (4, one per key), correct (0-3), back (explanation shown after the pick)
"""
import math

from PIL import Image, ImageDraw

from .render import font


# ------------------------------------------------------------------ images (drawn in code, 1-bit)
def ecg_flutter(w, h):
    """Atrial flutter with 4:1 block: sawtooth F waves at ~300/min, a narrow QRS every 4th wave."""
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    for x in range(0, w, 20):                       # light grid, dithered to dots later
        d.line([(x, 0), (x, h)], fill=225)
    for y in range(0, h, 20):
        d.line([(0, y), (w, y)], fill=225)
    base, amp, period = h * 0.62, 14, 40            # 40 px per F wave (0.2 s at 200 px/s)
    pts = []
    for x in range(w):
        ph = (x % period) / period
        y = base - amp * 0.5 + amp * ph if ph < 0.85 else base + amp * 0.5 - amp * (ph - 0.85) / 0.15
        beat = (x - 30) % (4 * period)
        if 0 <= beat < 16:                          # narrow QRS on every 4th flutter wave
            q = beat / 16
            y = base + (8 * math.sin(q * math.pi * 3)) - (h * 0.5 if 0.3 < q < 0.55 else 0) * math.sin((q - 0.3) / 0.25 * math.pi)
        pts.append((x, y))
    d.line(pts, fill=0, width=3)
    return img.convert("1", dither=Image.Dither.FLOYDSTEINBERG)


def pedigree_ad(w, h):
    """Three generations, every generation affected, both sexes, male-to-male transmission."""
    img = Image.new("1", (w, h), 1)
    d = ImageDraw.Draw(img)
    s = 26

    def male(x, y, aff):
        d.rectangle([x - s // 2, y - s // 2, x + s // 2, y + s // 2], outline=0, width=3, fill=0 if aff else 1)

    def female(x, y, aff):
        d.ellipse([x - s // 2, y - s // 2, x + s // 2, y + s // 2], outline=0, width=3, fill=0 if aff else 1)

    y1, y2, y3 = 30, h // 2, h - 30
    cx = w // 2
    male(cx - 60, y1, True); female(cx + 60, y1, False)
    d.line([(cx - 47, y1), (cx + 47, y1)], fill=0, width=3)
    d.line([(cx, y1), (cx, y1 + 40)], fill=0, width=3)
    kids = [cx - 150, cx - 40, cx + 70]
    d.line([(kids[0], y1 + 40), (kids[-1], y1 + 40)], fill=0, width=3)
    for k in kids:
        d.line([(k, y1 + 40), (k, y2 - s // 2)], fill=0, width=3)
    male(kids[0], y2, True); female(kids[1], y2, False); female(kids[2], y2, True)
    female(kids[0] - 110, y2, False)
    d.line([(kids[0] - 97, y2), (kids[0] - 13, y2)], fill=0, width=3)
    mid = kids[0] - 55
    d.line([(mid, y2), (mid, y2 + 40)], fill=0, width=3)
    g3 = [mid - 60, mid + 60]
    d.line([(g3[0], y2 + 40), (g3[1], y2 + 40)], fill=0, width=3)
    for k in g3:
        d.line([(k, y2 + 40), (k, y3 - s // 2)], fill=0, width=3)
    male(g3[0], y3, True); female(g3[1], y3, False)
    for label, y in (("I", y1), ("II", y2), ("III", y3)):
        d.text((6, y - 11), label, font=font(17, True), fill=0)
    return img


def target_lesion(w, h):
    """Dithered target lesion: dusky centre, pale ring, darker outer ring, on lightly shaded skin."""
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    cx, cy = w // 2, h // 2
    r = min(w, h) // 2 - 6
    d.ellipse([cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4], fill=215)
    for rr, shade in ((r, 120), (int(r * 0.72), 200), (int(r * 0.42), 95), (int(r * 0.22), 55)):
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=shade)
    return img.convert("1", dither=Image.Dither.FLOYDSTEINBERG)


def _ink(img):
    """Grey drawing with anti-aliased text to 1-bit, the same cut the card renderer uses."""
    return img.point(lambda v: 0 if v < 150 else 255, mode="1")


def _table(w, h, rows, col_w, head_h, row_h, head_col=False):
    """A ruled table: the first row is the header, and with head_col the first column is too."""
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    fh, fv = font(19), font(28, True)
    tw = sum(col_w)
    y = 1
    for r, row in enumerate(rows):
        rh = head_h if r == 0 else row_h
        x = 1
        for c, text in enumerate(row):
            f = fh if r == 0 or (head_col and c == 0) else fv
            d.text((x + (col_w[c] - f.getlength(text)) / 2, y + (rh - f.size) / 2 - 2), text, font=f, fill=0)
            x += col_w[c]
        y += rh
        if r < len(rows) - 1:
            d.line([(1, y), (tw, y)], fill=0, width=2)
    d.rectangle([0, 0, tw + 1, y + 1], outline=0, width=2)
    x = 1
    for cw in col_w[:-1]:
        x += cw
        d.line([(x, 0), (x, y)], fill=0, width=2)
    return _ink(img)


def abg_table(w, h):
    """An arterial blood gas with sodium and chloride: an anion gap acidosis, compensated as expected."""
    return _table(w, h, [["pH", "PaCO₂", "HCO₃⁻", "Na⁺", "Cl⁻"], ["7.25", "28", "12", "140", "104"]],
                  [116] * 5, 40, 52)


def two_by_two(w, h):
    """A screening test against the true diagnosis: 90 true and 40 false positives, 10 false and 160 true negatives."""
    return _table(w, h, [["", "Disease", "No disease"], ["Test positive", "90", "40"], ["Test negative", "10", "160"]],
                  [150, 140, 140], 38, 46, head_col=True)


def lineweaver_burk(w, h):
    """1/V against 1/[S]: no inhibitor (solid) and a competitive inhibitor (dashed), meeting on the y-axis."""
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    f = font(17)
    ox, oy, y_int = 150, h - 32, h - 62           # origin, and where both lines cross the y-axis (1/Vmax)
    d.line([(10, oy), (470, oy)], fill=0, width=2)
    d.line([(ox, oy + 6), (ox, 4)], fill=0, width=2)
    d.text((ox + 8, 0), "1/V", font=f, fill=0)
    d.text((440, oy + 6), "1/[S]", font=f, fill=0)
    d.text((ox + 10, y_int + 6), "1/Vmax", font=f, fill=0)

    def line(x_int, x_end, dashed):               # from the x-intercept up through (ox, y_int)
        slope = (oy - y_int) / (ox - x_int)
        n = 40
        for i in range(n):
            if dashed and i % 2:
                continue
            xa, xb = x_int + (x_end - x_int) * i / n, x_int + (x_end - x_int) * (i + 1) / n
            d.line([(xa, oy - slope * (xa - x_int)), (xb, oy - slope * (xb - x_int))], fill=0, width=3)

    line(ox - 100, 460, False)
    line(ox - 40, ox + 110, True)
    for y, dashed, label in ((40, False, "no inhibitor"), (72, True, "with inhibitor")):
        for i in range(0, 44, 11 if dashed else 44):
            d.line([(520 + i, y), (520 + i + (6 if dashed else 44), y)], fill=0, width=3)
        d.text((574, y - 11), label, font=f, fill=0)
    return _ink(img)


# ------------------------------------------------------------------ cards
CARDS = [
    # pediatrics
    dict(id="c01", kind="basic", front="Most common cause of bronchiolitis in infants?",
         back="RSV. It peaks in winter and mostly affects children under 2."),
    dict(id="c02", kind="basic", front="Treatment of Kawasaki disease?",
         back="IVIG plus high-dose aspirin, ideally within 10 days of fever onset, to prevent coronary artery aneurysms."),
    dict(id="c03", kind="basic", front="Diagnostic criteria for Kawasaki disease?",
         back="Fever for at least 5 days plus 4 of 5: bilateral nonexudative conjunctivitis, oral changes (strawberry tongue, "
              "red cracked lips), cervical lymphadenopathy of at least 1.5 cm (usually one side), a polymorphous rash, and "
              "extremity changes (swelling or redness of the hands and feet, then peeling of the fingertips)."),
    dict(id="c04", kind="basic", front="Steeple sign on a neck X-ray?",
         back="Croup (laryngotracheobronchitis), most often from parainfluenza virus."),
    dict(id="c05", kind="basic", front="Thumbprint sign on a lateral neck X-ray?", back="Epiglottitis."),
    dict(id="c06", kind="basic", front="First-line treatment for moderate croup?",
         back="A single dose of dexamethasone, plus nebulized racemic epinephrine if there is stridor at rest."),
    dict(id="c07", kind="basic", front="Classic triad of intussusception?",
         back="Intermittent colicky abdominal pain, vomiting and currant jelly stool. Ultrasound shows a target sign."),
    dict(id="c08", kind="basic",
         front="A 4-week-old has projectile nonbilious vomiting and an olive-shaped epigastric mass. Diagnosis?",
         back="Hypertrophic pyloric stenosis, with a hypochloremic hypokalemic metabolic alkalosis."),
    dict(id="c09", kind="basic", front="Typical age a child starts walking independently?",
         back="About 12 months. Not walking by 18 months needs evaluation."),
    dict(id="c10", kind="basic", front="Most common bacterial cause of acute otitis media?",
         back="Streptococcus pneumoniae, then nontypeable H. influenzae and Moraxella catarrhalis."),
    dict(id="c11", kind="basic", front="First-line antibiotic for acute otitis media?", back="High-dose amoxicillin."),
    # cardiology and medicine
    dict(id="c12", kind="basic", front="Drug of choice for stable narrow-complex SVT when vagal maneuvers fail?",
         back="Adenosine, as a rapid IV push followed by a saline flush."),
    dict(id="c13", kind="basic", front="Beck's triad?",
         back="Hypotension, distended neck veins and muffled heart sounds, seen in cardiac tamponade."),
    dict(id="c14", kind="basic", front="What does electrical alternans on an ECG suggest?",
         back="A large pericardial effusion, often with tamponade."),
    dict(id="c15", kind="basic", front="Reversal of warfarin with major bleeding?",
         back="4-factor prothrombin complex concentrate plus IV vitamin K."),
    dict(id="c16", kind="basic", front="Reversal agent for dabigatran?", back="Idarucizumab."),
    dict(id="c17", kind="basic", front="Reversal agent for heparin?", back="Protamine sulfate."),
    dict(id="c18", kind="basic", front="Classic acid-base picture in salicylate toxicity?",
         back="A mixed respiratory alkalosis and anion gap metabolic acidosis."),
    dict(id="c19", kind="basic", front="In DKA, when can the insulin infusion start?",
         back="After fluids are running and once potassium is at least 3.3 mEq/L."),
    # infectious disease
    dict(id="c20", kind="basic", front="Treatment of neurosyphilis?", back="IV aqueous penicillin G."),
    dict(id="c21", kind="basic", front="Multiple ring-enhancing brain lesions in a patient with AIDS and CD4 under 100?",
         back="Toxoplasmosis. A single lesion with EBV in the CSF points to primary CNS lymphoma instead."),
    dict(id="c22", kind="basic", front="When does PCP prophylaxis start in HIV, and with what?",
         back="CD4 below 200 cells per microliter, with TMP-SMX."),
    # psychiatry and pharmacology
    dict(id="c23", kind="basic", front="First-line treatment for opioid use disorder in pregnancy?",
         back="Buprenorphine or methadone."),
    dict(id="c24", kind="basic", front="Signs of lithium toxicity?",
         back="Coarse tremor, ataxia, confusion, vomiting and diarrhea, which can progress to seizures."),
    dict(id="c25", kind="basic", front="Serotonin syndrome or NMS: the key exam difference?",
         back="Serotonin syndrome has hyperreflexia and clonus and comes on within hours. NMS has lead-pipe rigidity and develops over days."),
    dict(id="c26", kind="basic", front="Main mechanism of metformin?",
         back="It lowers hepatic gluconeogenesis."),
    dict(id="c27", kind="basic", front="Which ACE inhibitor side effects come from bradykinin?",
         back="Dry cough and angioedema."),
    dict(id="c28", kind="basic", front="Flushing and rash during a fast vancomycin infusion?",
         back="Vancomycin infusion reaction (red man syndrome). Slow the infusion and give an antihistamine."),
    # longer vignettes, lists, comparisons and formulas
    dict(id="c29", kind="basic",
         front="A 67-year-old man with a 50 pack-year smoking history has lost 8 kg over 3 months. His sodium is "
               "124 mEq/L with a low serum osmolality, concentrated urine and a normal volume status. Which lung "
               "tumor is most likely?",
         back="Small cell carcinoma, causing SIADH."),
    dict(id="c30", kind="basic",
         front="A 32-year-old at 34 weeks of pregnancy has a headache, right upper quadrant pain and a blood pressure "
               "of 168/112. Platelets are 85,000, AST and ALT are three times normal, and the smear shows "
               "schistocytes. Diagnosis, and the definitive treatment?",
         back="HELLP syndrome: hemolysis, elevated liver enzymes and low platelets, a severe form of preeclampsia. "
              "Definitive treatment is delivery. Give IV magnesium sulfate to prevent seizures, and IV labetalol or "
              "hydralazine for the severe-range blood pressure."),
    dict(id="c31", kind="basic", front="Causes of an anion gap metabolic acidosis (MUDPILES)?",
         back="• Methanol\n• Uremia\n• Diabetic ketoacidosis\n• Propylene glycol\n• Iron and isoniazid\n"
              "• Lactic acidosis\n• Ethylene glycol\n• Salicylates"),
    dict(id="c32", kind="basic", front="Nephritic or nephrotic syndrome: how do they differ?",
         back="Nephritic: hematuria with RBC casts, hypertension, oliguria, and protein under 3.5 g a day.\n\n"
              "Nephrotic: protein over 3.5 g a day, low albumin, edema, high lipids and a tendency to clot."),
    dict(id="c33", kind="basic", front="Winter's formula?",
         back="Expected PaCO₂ = 1.5 × HCO₃⁻ + 8 (± 2).\n\nHigher than that means a respiratory acidosis on top, "
              "lower means a respiratory alkalosis on top."),
    dict(id="c34", kind="basic", front="Most common site of an ectopic pregnancy?", back="The ampulla of the fallopian tube."),
    dict(id="c35", kind="basic", front="Suspected acute ischemic stroke, 2 hours after onset. What happens first?",
         back="A noncontrast head CT straight away to rule out bleeding, and a fingerstick glucose.\n\n"
              "No bleed and no contraindication: IV alteplase or tenecteplase within 4.5 hours of onset, with the "
              "blood pressure under 185/110 before and under 180/105 after.\n\n"
              "CT angiography looks for a large vessel occlusion, which can go to thrombectomy up to 24 hours out "
              "in selected patients."),
    dict(id="c36", kind="basic", front="Peaked T waves on the ECG. What is the problem, and what is given first?",
         back="Hyperkalemia. IV calcium gluconate first to stabilize the heart, then insulin with glucose to shift "
              "potassium into cells."),
    dict(id="c37", kind="basic",
         front="A competent adult refuses a blood transfusion for religious reasons. What should you do?",
         back="Respect the refusal. A competent adult can refuse any treatment, even one that would save their life."),
    # multiple choice, one option above each key
    dict(id="m01", kind="choice", front="A 2-year-old has a barking cough and inspiratory stridor. Most likely cause?",
         options=["Parainfluenza virus", "RSV", "H. influenzae type b", "Group A strep"], correct=0,
         back="Croup. Parainfluenza is the classic cause, with a steeple sign on X-ray."),
    dict(id="m02", kind="choice", front="Which lab pattern fits pyloric stenosis?",
         options=["Hyperchloremic acidosis", "Hypochloremic alkalosis", "Hyperkalemia", "Anion gap acidosis"], correct=1,
         back="Vomiting gastric acid causes a hypochloremic hypokalemic metabolic alkalosis."),
    dict(id="m03", kind="choice", front="First-line drug for childhood absence seizures?",
         options=["Ethosuximide", "Phenytoin", "Carbamazepine", "Gabapentin"], correct=0,
         back="Ethosuximide. Carbamazepine and phenytoin can make absence seizures worse."),
    dict(id="m04", kind="choice", front="Which of these vaccines is live?",
         options=["MMR", "Hepatitis B", "Tdap", "Pneumococcal conjugate"], correct=0,
         back="MMR is live attenuated, so it is avoided in pregnancy and severe immunosuppression."),
    dict(id="m05", kind="choice", front="First drug to give in anaphylaxis?",
         options=["IM epinephrine", "IV diphenhydramine", "Oral prednisone", "Nebulized albuterol"], correct=0,
         back="IM epinephrine into the lateral thigh. Antihistamines and steroids are only adjuncts."),
    dict(id="m06", kind="choice", front="Most common congenital heart defect?",
         options=["VSD", "ASD", "Tetralogy of Fallot", "PDA"], correct=0,
         back="Ventricular septal defect. Many small ones close on their own."),
    dict(id="m07", kind="choice",
         front="A 19-year-old college student has had fever, headache and confusion for a day. She has neck stiffness "
               "and a petechial rash on the trunk and legs. The CSF Gram stain shows gram-negative diplococci. Most "
               "likely organism?",
         options=["N. meningitidis", "S. pneumoniae", "Listeria", "H. influenzae"], correct=0,
         back="Neisseria meningitidis. Close contacts get prophylaxis with rifampin, ciprofloxacin or ceftriaxone."),
    dict(id="m08", kind="choice",
         front="A 70-year-old has sudden painless vision loss in one eye. The fundus shows a pale retina with a "
               "cherry-red spot. Best next step?",
         options=["Urgent stroke workup at a stroke center", "Topical timolol and pilocarpine",
                  "Intravitreal anti-VEGF injection", "Laser photocoagulation of the retina"], correct=0,
         back="Central retinal artery occlusion is a stroke of the eye: carotid imaging, heart rhythm and an echo, "
              "urgently. Check ESR and CRP too, since giant cell arteritis can cause it."),
    dict(id="m09", kind="choice",
         front="A 61-year-old man has had crushing chest pain for 40 minutes. The ECG shows ST elevation in II, III "
               "and aVF, and his blood pressure falls to 82/50 after one dose of sublingual nitroglycerin. Which "
               "other finding is most likely?",
         options=["ST elevation in V4R", "A new left bundle branch block", "Diffuse ST elevation with PR depression",
                  "Deep T wave inversions in V2 and V3"], correct=0,
         back="An inferior MI with right ventricular involvement. The right ventricle depends on preload, so nitrates "
              "drop the pressure: give IV fluids, and avoid nitrates and diuretics."),
    dict(id="m10", kind="choice", front="Target INR for a mechanical mitral valve?",
         options=["2.0 to 3.0", "2.5 to 3.5", "1.5 to 2.0", "3.5 to 4.5"], correct=1,
         back="2.5 to 3.5. A mechanical aortic valve with no other risk factors aims for 2.0 to 3.0."),
    dict(id="m11", kind="choice",
         front="A 28-year-old woman has palpitations, weight loss and heat intolerance, and her eyes protrude. TSH is "
               "undetectable, free T4 is high, and radioactive iodine uptake is high and diffuse. Diagnosis?",
         options=["Graves disease", "Toxic multinodular goiter", "Subacute thyroiditis", "Factitious thyrotoxicosis"],
         correct=0,
         back="Graves disease: diffuse high uptake plus eye signs. Thyroiditis and taking thyroid hormone both show "
              "low uptake, and a toxic goiter takes it up in patches."),
    dict(id="m12", kind="choice", front="Preferred antihypertensive in diabetes with albuminuria?",
         options=["ACE inhibitor", "Thiazide diuretic", "Amlodipine", "Metoprolol"], correct=0,
         back="An ACE inhibitor, or an ARB if the ACE inhibitor causes a cough. Both lower the pressure inside the "
              "glomerulus and slow diabetic kidney disease. Recheck potassium and creatinine 1 to 2 weeks after "
              "starting, since both can rise."),
    dict(id="m13", kind="choice",
         front="A patient on haloperidol has a temperature of 40.5 °C, lead-pipe rigidity and a CK of 9,000. Besides "
               "stopping the drug, which treatment helps?",
         options=["Dantrolene", "Cyproheptadine", "Physostigmine", "Naloxone"], correct=0,
         back="Neuroleptic malignant syndrome. Cool, hydrate, and give dantrolene or bromocriptine. Cyproheptadine is "
              "for serotonin syndrome."),
    # image cards
    dict(id="i01", kind="basic", front="Name this rhythm.", image=ecg_flutter, image_h=150,
         back="Atrial flutter with 4:1 block: sawtooth flutter waves near 300 per minute and a ventricular rate around 75."),
    dict(id="i02", kind="basic", front="Most likely inheritance pattern?", image=pedigree_ad, image_h=190,
         back="Autosomal dominant. Every generation is affected, both sexes, and male-to-male transmission rules out X-linked."),
    dict(id="i03", kind="basic", front="This skin lesion pattern is classic for what?", image=target_lesion, image_h=170,
         back="Erythema multiforme (target lesions), most often triggered by HSV."),
    dict(id="i04", kind="basic", front="Interpret these results.", image=abg_table, image_h=96,
         back="An anion gap metabolic acidosis (gap of 140 − 104 − 12 = 24) with appropriate respiratory "
              "compensation: Winter's formula predicts a PaCO₂ of 26 ± 2."),
    dict(id="i05", kind="basic", front="What is the sensitivity of this new test?", image=two_by_two, image_h=134,
         back="90%: true positives over everyone with the disease, 90 / (90 + 10). Its specificity is 160 / 200 = 80%."),
    dict(id="i06", kind="basic", front="What kind of inhibitor gives the dashed line?", image=lineweaver_burk,
         image_h=170, back="A competitive inhibitor. The lines meet on the y-axis, so Vmax is unchanged, and Km goes up."),
]

EXTRA_CARDS = [   # the dashboard's "Add a card" button draws from these
    dict(kind="basic", front="Antidote for acetaminophen overdose?", back="N-acetylcysteine."),
    dict(kind="basic", front="Most common cause of community-acquired pneumonia?", back="Streptococcus pneumoniae."),
    dict(kind="basic", front="Koplik spots are seen in?", back="Measles (rubeola), before the rash appears."),
    dict(kind="basic", front="Slapped cheek rash in a child?", back="Erythema infectiosum (fifth disease), from parvovirus B19."),
    dict(kind="basic", front="Which mood stabilizer is linked to Ebstein anomaly when taken in pregnancy?",
         back="Lithium."),
    dict(kind="basic",
         front="A 40-year-old woman has fatigue, joint pain and a malar rash that spares the nasolabial folds. Which "
               "antibodies are most specific for her likely diagnosis?",
         back="Anti-double-stranded DNA and anti-Smith, for lupus. ANA is sensitive but not specific."),
    dict(kind="basic", front="A newborn isn't breathing at birth. The first steps of resuscitation?",
         back="Warm, dry and stimulate the baby, open the airway, and suction only if needed.\n\n"
              "Still apneic, or a heart rate under 100: positive pressure ventilation within the first minute.\n\n"
              "Heart rate under 60 despite good ventilation: chest compressions at 3 to 1, with 100% oxygen."),
]
